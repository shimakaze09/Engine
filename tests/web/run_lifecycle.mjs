// Headless-Chromium driver for the web build's browser tests (#341, #652).
// It serves a web build directory with the COOP/COEP headers the engine's
// threads need, loads a page on SwiftShader's WebGL2, and checks one case:
//
//   frames, max_frames_1, quit, fatal_stage
//       engine_web_lifecycle.html: the run's loop ends inside web_frame,
//       every engine tier is closed afterwards, the editor bridge player
//       mode displaced is back, and a second bootstrap in the same page
//       starts, runs and closes every tier again.
//   page_boots
//       engine_editor_app.html, the shipped page: it boots in player mode
//       and runs frames with no page error and no engine error line.
//
// Usage: node run_lifecycle.mjs --dir <web build dir> --case <case>
// Waits are hang guards, never assertions: every check is on state the
// page reports, not on how long it took.

import { chromium } from 'playwright';
import { createReadStream, existsSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize, sep } from 'node:path';

const kHangGuardMs = 120000;
// The tiers web_lifecycle_test.cpp probes, one bit each, in its order.
const kAllTiers = (1 << 6) - 1;
const kContentTypes = {
  '.html': 'text/html',
  '.js': 'text/javascript',
  '.wasm': 'application/wasm',
  '.data': 'application/octet-stream',
};

function parseArgs(argv) {
  const args = {};
  for (let i = 2; i + 1 < argv.length; i += 2) {
    args[argv[i].replace(/^--/, '')] = argv[i + 1];
  }
  return args;
}

/// Static server over `root` with cross-origin isolation, so the page gets
/// SharedArrayBuffer for its pthreads.
function serve(root) {
  const server = createServer((request, response) => {
    const url = new URL(request.url, 'http://localhost');
    const file = normalize(join(root, decodeURIComponent(url.pathname)));
    if (!file.startsWith(normalize(root) + sep) || !existsSync(file) ||
        !statSync(file).isFile()) {
      response.writeHead(404);
      response.end();
      return;
    }
    response.writeHead(200, {
      'Content-Type': kContentTypes[extname(file)] ?? 'application/octet-stream',
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cache-Control': 'no-store',
    });
    createReadStream(file).pipe(response);
  });
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => resolve(server));
  });
}

class Failure extends Error {}

function check(condition, message) {
  if (!condition) {
    throw new Failure(message);
  }
}

/// Console capture with waits keyed on the lines the page prints.
class Console {
  constructor(page) {
    this.lines = [];
    this.pageErrors = [];
    this.waiters = [];
    page.on('console', (message) => this.push(message.text()));
    page.on('pageerror', (error) => {
      this.pageErrors.push(String(error.stack ?? error));
      this.push(`[pageerror] ${error}`);
    });
  }

  push(line) {
    this.lines.push(line);
    this.waiters = this.waiters.filter((waiter) => !waiter(line));
  }

  /// Resolves with the first line matching `pattern`; rejects on a line
  /// matching `failure`, a page error, or the hang guard.
  waitFor(pattern, failure = null) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(
          () => reject(new Failure(`no line matching ${pattern}`)),
          kHangGuardMs);
      const test = (line) => {
        if (pattern.test(line)) {
          clearTimeout(timer);
          resolve(line);
          return true;
        }
        if ((failure && failure.test(line)) || line.startsWith('[pageerror]')) {
          clearTimeout(timer);
          reject(new Failure(`page reported: ${line}`));
          return true;
        }
        return false;
      };
      if (!this.lines.some(test)) {
        this.waiters.push(test);
      }
    });
  }

  /// Engine log lines at Error level that `allowed` does not name, and
  /// every WebGL error: a rejected call is a draw or an upload that never
  /// happened. bgfx's format-capability probe at init asks
  /// getInternalformatParameter about targets WebGL2 lacks; those
  /// INVALID_ENUM lines are the probe's answers, not failures.
  unexpectedErrors(allowed) {
    return this.lines.filter((line) =>
        (/\]\[Error\]\[/.test(line) && !allowed.some((a) => a.test(line))) ||
        (/GL_INVALID_|WebGL: INVALID_|CONTEXT_LOST/.test(line) &&
         !/getInternalformatParameter/.test(line)));
  }
}

const kRunFailure = /\[web-lifecycle\] (bootstrap refused|run refused|unknown case)/;

function maskOf(line) {
  const match = /mask=(\d+) open=(\S+)/.exec(line);
  return { mask: Number(match[1]), open: match[2] };
}

/// Waits for the current run's loop to end, then reads the tiers left
/// open after it through the harness.
async function closeOut(page, log, run) {
  await page.waitForFunction(() => Module._web_lifecycle_loop_ended() === 1,
                             null, { polling: 'raf', timeout: kHangGuardMs });
  const mask = await page.evaluate((r) => Module._web_lifecycle_report(r), run);
  const tiers = maskOf(await log.waitFor(
      new RegExp(`\\[web-lifecycle\\] tiers after run=${run} `)));
  const bridge = await log.waitFor(
      new RegExp(`\\[web-lifecycle\\] bridge run=${run} `));
  check(mask === 0 && tiers.mask === 0,
        `run ${run}: tiers still open after its loop ended: ${tiers.open}`);
  check(bridge.endsWith('restored=1'),
        `run ${run}: the editor bridge player mode displaced is not back`);
}

async function startedRun(log, run) {
  const started = new RegExp(`\\[web-lifecycle\\] run started run=${run} `);
  const opened = maskOf(await log.waitFor(
      new RegExp(`\\[web-lifecycle\\] tiers opened run=${run} `), kRunFailure));
  check(opened.mask === kAllTiers,
        `run ${run}: bootstrap left tiers unopened (open: ${opened.open}), ` +
        'so the closing probe would prove nothing');
  await log.waitFor(started, kRunFailure);
}

async function lifecycleCase(page, log, base, name) {
  await page.goto(`${base}/engine_web_lifecycle.html?case=${name}`);
  await log.waitFor(new RegExp(`\\[web-lifecycle\\] case=${name}$`),
                    kRunFailure);
  await startedRun(log, 1);
  if (name === 'quit') {
    // Quit mid-run, once frames are flowing, not before the first one.
    await page.waitForFunction(() => Module._web_lifecycle_frame() >= 3, null,
                               { polling: 'raf', timeout: kHangGuardMs });
    await page.evaluate(() => Module._web_lifecycle_request_quit());
  }
  await closeOut(page, log, 1);
  const fatalLines = log.lines.filter((l) => /fatal frame error/.test(l));
  if (name === 'fatal_stage') {
    check(log.lines.some((l) => /injected frame-stage failure/.test(l)),
          'the injected stage failure never fired');
    check(fatalLines.length === 1, 'the fatal frame did not stop run 1');
  } else {
    check(fatalLines.length === 0, 'run 1 stopped on a fatal frame');
  }

  // A second bootstrap in the same page sees none of run 1's state: it
  // opens every tier again, runs, and closes them all once more.
  await page.evaluate(() => Module._web_lifecycle_second_run());
  await startedRun(log, 2);
  await closeOut(page, log, 2);
  check(log.lines.filter((l) => /fatal frame error/.test(l)).length ===
            fatalLines.length,
        'run 2 stopped on a fatal frame');

  const allowed = name === 'fatal_stage'
      ? [/injected frame-stage failure/, /engine stopped on a fatal frame error/]
      : [];
  const errors = log.unexpectedErrors(allowed);
  check(errors.length === 0, `engine errors: ${errors.join(' | ')}`);
}

async function pageBootsCase(page, log, base) {
  await page.goto(`${base}/engine_editor_app.html`);
  await log.waitFor(/\[engine\] bootstrap complete/);
  check(log.lines.some((l) => /environment override: app\.player_mode = 1/.test(l)),
        'the share shell did not seed player mode');
  // One engine frame per animation frame: 120 of them is past startup
  // and asset streaming into steady-state play.
  await page.evaluate(async () => {
    for (let i = 0; i < 120; ++i) {
      await new Promise((resolve) => requestAnimationFrame(resolve));
    }
  });
  const errors = log.unexpectedErrors([]);
  check(errors.length === 0, `engine errors: ${errors.join(' | ')}`);
}

async function main() {
  const args = parseArgs(process.argv);
  check(args.dir && args.case, 'usage: --dir <web build dir> --case <case>');
  const server = await serve(args.dir);
  const base = `http://127.0.0.1:${server.address().port}`;
  // SwiftShader everywhere, so a GPU host and a GPU-less CI runner run the
  // same WebGL2 implementation; audio starts without a gesture so the
  // WebAudio callback path runs too.
  const browser = await chromium.launch({
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader',
           '--autoplay-policy=no-user-gesture-required'],
  });
  const page = await browser.newPage();
  // The engine sizes its thread pools from this, and a CI runner reports
  // four: pinning a many-core value makes every host ask for more threads
  // than the page's worker budget, the case that once hung a shutdown.
  // ENGINE_WEB_TEST_CORES overrides it for diagnosis.
  await page.addInitScript((cores) => {
    Object.defineProperty(navigator, 'hardwareConcurrency', { get: () => cores });
  }, Number(process.env.ENGINE_WEB_TEST_CORES ?? 16));
  const log = new Console(page);
  let failure = null;
  try {
    if (args.case === 'page_boots') {
      await pageBootsCase(page, log, base);
    } else {
      await lifecycleCase(page, log, base, args.case);
    }
    check(log.pageErrors.length === 0,
          `page errors: ${log.pageErrors.join(' | ')}`);
  } catch (error) {
    failure = error;
  }
  if (failure) {
    console.log(log.lines.slice(-80).join('\n'));
    for (const stack of log.pageErrors) {
      console.log(`page error stack: ${stack}`);
    }
    console.log(`FAIL ${args.case}: ${failure.message}`);
  } else {
    console.log(`PASS ${args.case}`);
  }
  // A page whose main thread spins in a wait never answers a close, so
  // the close is bounded and the verdict above stands either way.
  await Promise.race([browser.close(),
                      new Promise((resolve) => setTimeout(resolve, 10000))]);
  server.close();
  process.exit(failure ? 1 : 0);
}

await main();
