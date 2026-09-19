# Regression for #351: the generators publish a set behind a manifest the
# asset packer validates, so a publish interrupted between file
# replacements can never be cooked as a mixed generation. Drives the
# production gen_props.py with its fault-injection hook and the production
# asset_packer against a scratch copy of the prop set.
#
#   python3 tools/test_gen_publish.py <path/to/asset_packer>
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import gen_common  # noqa: E402

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")


def run_generator(out_dir, fail_after=None):
    env = dict(os.environ)
    env.pop(gen_common.FAIL_AFTER_ENV, None)
    if fail_after is not None:
        env[gen_common.FAIL_AFTER_ENV] = str(fail_after)
    proc = subprocess.run([sys.executable, str(TOOLS / "gen_props.py"),
                           str(out_dir)], capture_output=True, text=True,
                          env=env)
    return proc.returncode


def cook(packer, source, output):
    proc = subprocess.run([packer, str(source), str(output), "--force"],
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def main():
    if len(sys.argv) < 2:
        print("usage: test_gen_publish.py <asset_packer>")
        return 2
    packer = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp) / "props"
        cooked = Path(tmp) / "cooked"
        cooked.mkdir()

        # Generation 1: a clean publish cooks.
        check(run_generator(out_dir) == 0, "generator publishes")
        manifest = out_dir / gen_common.MANIFEST_NAME
        check(manifest.exists(), "manifest written")
        entries = gen_common.load_manifest(str(out_dir))
        names = sorted(entries)
        check(len(names) >= 4, "manifest lists the prop set")
        gltf_names = [n for n in names if n.endswith(".gltf")]
        first = gltf_names[0]
        code, _ = cook(packer, out_dir / first, cooked / "first.mesh")
        check(code == 0, "clean generation cooks")

        # Make the on-disk set a self-consistent *previous* generation that
        # differs from what the generator produces: alter one glTF's
        # generator string and re-certify it in the manifest.
        target = out_dir / first
        doc = json.loads(target.read_text(encoding="utf-8"))
        doc["asset"]["generator"] = "previous generation"
        target.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n",
                          encoding="utf-8", newline="\n")
        data = target.read_bytes()
        entries[first] = {"bytes": len(data),
                          "fnv1a64": f"{gen_common.fnv1a64(data):016x}"}
        gen_common.write_manifest(str(out_dir), entries)
        code, _ = cook(packer, target, cooked / "previous.mesh")
        check(code == 0, "self-consistent previous generation cooks")

        # Generation 2 interrupted after every replacement but the last:
        # the altered glTF now carries new bytes beside the previous
        # manifest. It must be refused, not cooked as a mixed generation.
        staged_count = len(names)
        check(run_generator(out_dir, fail_after=staged_count - 1) != 0,
              "interrupted publish reports failure")
        after = gen_common.load_manifest(str(out_dir))
        check(after == entries, "interrupted publish leaves the previous "
                                "manifest in place")
        code, output = cook(packer, target, cooked / "mixed.mesh")
        check(code != 0, "mixed generation refused by the packer")
        check("mixed generation" in output,
              "refusal names the interrupted publish")
        check(not (cooked / "mixed.mesh").exists(),
              "no cooked output for the mixed generation")

        # Completing the publish certifies the whole new set again.
        check(run_generator(out_dir) == 0, "re-run completes the publish")
        code, _ = cook(packer, target, cooked / "complete.mesh")
        check(code == 0, "completed generation cooks")
        leftovers = [p for p in out_dir.iterdir() if p.suffix == ".tmp"]
        check(not leftovers, "completed publish leaves no staged files")

    if failures:
        print(f"test_gen_publish: {len(failures)} failure(s)")
        return 1
    print("test_gen_publish: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
