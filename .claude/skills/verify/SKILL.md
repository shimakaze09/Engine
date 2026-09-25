---
name: verify
description: >
  Engine's verification procedure, tiered by what a change touches. Load it
  before claiming any change is verified, before writing a pull request's
  evidence section, and before reporting a fix complete. Load it especially
  when a change touches the renderer, shaders, serialization, physics, the
  job graph, fixed-capacity storage, authored-file writes, or the Lua API,
  because each adds a required step the baseline build-and-test does not
  cover. A renderer change carries a requirement CI cannot satisfy.
---

# Verification

A change is verified when the baseline passes **and** every tier matching
what the change touched passes. Report the tiers you ran with the platform
and configuration. What you did not run is not verified: say so instead of
widening the claim.

## Baseline — every change

```bash
cmake --build build --parallel                              # warning-free, first-party
ctest --test-dir build --output-on-failure -LE gpu          # headless suite
python tools/check_source_comments.py
python tools/check_comment_quality.py
python tools/check_module_deps.py
python tools/check_dependency_pins.py
python tools/check_content_attributes.py
python tools/check_test_timing.py
python tools/check_error_handling.py
python tools/check_portable_fopen.py
python tools/check_duplicate_primitives.py
python tools/ci/check_asset_metadata_paths.py
python tools/check_asset_identity.py
python tools/check_shader_variants.py
python tools/check_doc_references.py
python tools/check_array_value_init.py
python tools/test_tool_gates.py                             # the gates' own tests
```

The list mirrors the static-checks job in `.github/workflows/ci.yml`; a
gate added there is added here in the same change.

**Documents move with the code.** Before every push, update each document
the change makes false -- `README.md`, `docs/`, `CLAUDE.md`, these skills,
and the header comments describing the changed behavior -- in the same
push. `check_doc_references.py` catches a named path, link or test that no
longer exists; reread the prose around what you changed for the rest.

From an agent harness or any process without its own console, wrap the
command so child processes do not flash windows:

```powershell
pwsh -NoProfile -File tools/run_quiet.ps1 -- ctest --test-dir build --output-on-failure -LE gpu
```

## Stop-the-line rules

These override every tier below.

- **A failing test is never normalized.** If a test fails for an
  environment reason, fix it or make it skip with a stated reason in the
  same change. Never carry a known failure into a report, a pull request
  body, or a template sentence. "N-1 of N pass, the one failure also fails
  on main" is not a passing suite — it is two defects, the original one and
  the habit.
- **A claim names its evidence.** "Verified", "landed" and
  "production-ready" require the commit, the test or observation that
  proves it, the platform, and the date. Without all four, describe what
  you actually did.
- **No feature is described as working anywhere** — a document, a comment,
  a pull request — without a named end-to-end test or a dated human
  observation beside it.

## Tier: renderer, shaders, post stack

**CI verifies this tier only partly.** Only the Linux, Windows and macOS
Release lanes build with the shader cook; every other lane passes
`ENGINE_BGFX_SHADERC=OFF`. The Linux Release lane runs the `gpu`-labelled
suites on Mesa's software Vulkan (lavapipe) under Xvfb, so a crash in the
device path, an unbound pass or a readback those suites assert fails
there. Nothing runs them on a hardware GPU, on D3D11/D3D12, Metal or
OpenGL, or looks at a frame. The web lane draws frames on SwiftShader's
WebGL2 but only fails on an error; it compares no image. Treat a green CI
run as evidence for what those suites assert, and nothing more.

Required:

1. Build with the cook on, so shaders compile and the cooked binaries
   exist:
   `cmake -S . -B build -DENGINE_BGFX_SHADERC=ON` then build. On macOS
   this needs Xcode 16 or newer.
2. Run the `gpu`-labelled suites: `ctest --test-dir build -L gpu`. With
   no GPU, CI's form runs them on lavapipe:
   `xvfb-run -a env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ctest --test-dir build -L gpu`
   (packages `xvfb`, `mesa-vulkan-drivers`). That is a software result,
   not the hardware observation of step 4.
3. Run the editor windowed and look at what you changed. Toggle it off and
   on. A feature you cannot see change is a feature you have not verified.
4. Record the observation: date, commit, GPU, driver, backend, what you saw.

If you cannot open a window — headless container, no GPU — you have not
verified a renderer change. State that, name the observation someone must
make, and do not claim the tier. This is the normal outcome in CI-like
environments and is not a reason to skip the tier or soften the claim.

## Tier: web platform

Anything behind `ENGINE_PLATFORM_WEB`, the Web link options, a web shell,
the lifecycle the browser loop drives, or a thread count (every web thread
comes from a fixed pool). The `web (Emscripten)` CI lane builds the pages
and runs `ctest -L web`; locally it needs emsdk 6.0.10 activated, a native
build's cook, and the Playwright pinned in `tests/web/`:

```bash
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DENGINE_TARGET_PLATFORM=Web -DENGINE_BUILD_TESTS=ON -DENGINE_BUILD_TOOLS=OFF -DENGINE_WEB_COOKED_DIR=<native-build>/assets/shaders/bgfx/cooked
cmake --build build-web --parallel
npm ci --prefix tests/web
npx --prefix tests/web playwright install chromium-headless-shell
ctest --test-dir build-web -L web --output-on-failure
```

The browser runs SwiftShader, so a green run shows the page runs without a
page, engine or WebGL error. It is not the renderer tier: a visible change
still needs the page looked at in a browser on a real GPU.

## Tier: serialization, scene, prefab, save, metadata

- Round-trip through the production entry point, not a copied model.
- A parse or load failure leaves the destination unchanged. Prove it.
- Byte-identical output for unchanged input (the deterministic-cook
  contract) where the format promises it.
- Pair with
  `-R 'determinism|scene_serializer|scene_version_gate|prefab|component_registry|reflect_wire_key|save_data'`.
- A format change carries a migration and a test that reads the old form.

## Tier: physics, math

- `-R 'physics|math|ccd|joint|collision|collider|determinism'` (one
  regex; CTest keeps only the last `-R`).
- Tolerances are justified absolute and/or relative bounds plus an
  invariant (energy, momentum, penetration depth). An arbitrary loose
  tolerance is a defect, not a passing test.
- Integer state and promised hashes are exact.

## Tier: job graph, frame, lifecycle

- Exercise the production pipeline at worker counts 0, 1 and N. A copied
  scheduler model is supplementary coverage and proves nothing about the
  real DAG.
- Cover zero jobs, disabled subsystems, submission failure, catch-up
  steps, and repeated run/world transitions.
- TSAN is supporting evidence, not proof of a happens-before relation.
  State the relation explicitly.

## Tier: fixed-capacity storage

Cover, every time: zero items, one item, many, exactly at capacity, one
past capacity, and handle reuse across the generation wrap. One past
capacity must do what the capacity policy says (see
`docs/architecture.md`), not whatever the call site happens to do.

## Tier: authored-file writes

Inject faults at write, flush, sync, close, rename, parse, restore and
rollback. A failed save leaves the previous valid state intact. A
multi-file output commits as a transaction or a manifest, so an
interruption cannot leave a mixed state.

## Tier: Lua API

- `-R 'lua|script|sandbox|hotreload|bindgen|coroutine'`.
- Validate stack usage; preserve traceback, sandbox and hot-reload
  behavior.
- An API change updates its `// LUA_BIND:` annotations in
  `scripting/include/engine/scripting/bindable_api.h` (the bindings
  regenerate at build) and the README's Lua scripting section in the same
  change.
