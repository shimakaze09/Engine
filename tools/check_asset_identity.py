#!/usr/bin/env python3
"""Audit asset identity: a committed sidecar, unique, and portable.

Three ways a project loses track of which asset is which, all of them
silent until somebody else clones the repository.

**A missing sidecar.** An asset's persistent GUID lives in its
"<asset>.meta". If that file is not committed, a teammate's clone has the
asset but not its identity, and the next import on their machine mints a
different GUID — so one asset ends up with two identities and every
reference resolves on one machine and not the other. A sidecar that
exists locally but is untracked or ignored looks fine to whoever created
it, which is exactly why this is a gate and not a review item.

**A duplicate GUID.** Copying an asset in a file manager copies its
sidecar, so two assets claim one identity and a reference to it resolves
to whichever the index happened to see last. Every colliding path is
reported and the audit fails; it never picks a winner and never
regenerates one of them, because either choice silently rebinds
references somebody already wrote.

**A case-only path collision.** "Foo.png" and "foo.png" are two assets on
Linux and one on Windows and macOS, so a project holding both builds for
one teammate and not another. Reported as the portability conflict it is
rather than folded away, since folding would make identity depend on
which machine indexed the project.

Identity-bearing is read from the asset type table's own rows rather than
restated here, so adding a type to the table extends this audit with it.

Usage:
  python tools/check_asset_identity.py            # report, exit 1 on findings
  python tools/check_asset_identity.py --root DIR # audit an alternate tree
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
TABLE_HEADER = "content/include/engine/content/asset_type_table.h"
ASSET_ROOT = "assets"
SIDECAR_SUFFIX = ".meta"

ROW_RE = re.compile(
    r"X\(\s*(\w+)\s*,\s*\"[^\"]*\"\s*,\s*(\w+)\s*,\s*\w+\s*,\s*\(([^)]*)\)\s*,"
    r"\s*\(([^)]*)\)\s*\)")


def identity_bearing_suffixes(root: pathlib.Path) -> list[str]:
    """Suffixes whose files own an identity, from the asset type table.

    A source type's authored suffix and a cooked type's source suffix: the
    authored file is what a human made and what a reference must survive a
    rename of. A cooked or derived output belongs to the source that
    produced it and owns no identity of its own.
    """
    text = (root / TABLE_HEADER).read_text(encoding="utf-8")
    body = text[text.index("#define ENGINE_ASSET_TYPE_TABLE"):]
    body = body[:body.index("\n\n")]
    suffixes: list[str] = []
    for tag, policy, sources, _cooked in ROW_RE.findall(body):
        if policy not in ("Source", "Cooked"):
            continue
        for quoted in re.findall(r"\"([^\"]+)\"", sources):
            suffixes.append(quoted.lower())
    if not suffixes:
        raise SystemExit("check_asset_identity: parsed no suffixes from the "
                         "asset type table; the gate would pass vacuously")
    return suffixes


def tracked_files(root: pathlib.Path) -> set[str]:
    """Every tracked path under the asset root, as posix strings."""
    result = subprocess.run(
        ["git", "ls-files", "-z", "--", ASSET_ROOT],
        cwd=root, capture_output=True, text=True, check=True)
    return {name for name in result.stdout.split("\0") if name}


GUID_RE = re.compile(r'"guid"\s*:\s*"([0-9a-fA-F-]{36})"')


def sidecar_guid(path: pathlib.Path) -> str | None:
    """The GUID a sidecar claims, lowercased; None when unreadable."""
    try:
        match = GUID_RE.search(path.read_text(encoding="utf-8"))
    except OSError:
        return None
    return match.group(1).lower() if match else None


def duplicate_guid_findings(root: pathlib.Path,
                            sidecars: list[str]) -> list[str]:
    """One finding per GUID claimed by more than one sidecar."""
    owners: dict[str, list[str]] = {}
    for name in sidecars:
        guid = sidecar_guid(root / name)
        if guid is None:
            continue
        owners.setdefault(guid, []).append(name)
    findings: list[str] = []
    for guid, paths in sorted(owners.items()):
        if len(paths) < 2:
            continue
        findings.append(f"  {guid} is claimed by {len(paths)} sidecars:")
        findings.extend(f"      {path}" for path in sorted(paths))
    return findings


def case_collision_findings(tracked: set[str]) -> list[str]:
    """One finding per set of tracked paths differing only by case."""
    folded: dict[str, list[str]] = {}
    for name in tracked:
        folded.setdefault(name.lower(), []).append(name)
    findings: list[str] = []
    for lowered, paths in sorted(folded.items()):
        if len(paths) < 2:
            continue
        findings.append(f"  {len(paths)} paths differ only by case "
                        f"(one file on Windows and macOS, {len(paths)} on "
                        f"Linux):")
        findings.extend(f"      {path}" for path in sorted(paths))
    return findings


def main() -> int:
    """Runs the audit and reports findings; exit 1 when any exist."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(REPO_ROOT))
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()

    suffixes = identity_bearing_suffixes(root)
    tracked = tracked_files(root)

    findings: list[str] = []
    sidecars = [name for name in tracked
                if name.lower().endswith(SIDECAR_SUFFIX)]
    findings.extend(duplicate_guid_findings(root, sidecars))
    findings.extend(case_collision_findings(tracked))

    audited = 0
    for name in sorted(tracked):
        lower = name.lower()
        if lower.endswith(SIDECAR_SUFFIX):
            continue
        if not any(lower.endswith(suffix) for suffix in suffixes):
            continue
        audited += 1
        sidecar = name + SIDECAR_SUFFIX
        if sidecar in tracked:
            continue
        on_disk = (root / sidecar).is_file()
        why = ("exists but is not tracked (check .gitignore)" if on_disk
               else "does not exist; run asset_packer --init-meta")
        findings.append(f"  {name}: {sidecar} {why}")

    if findings:
        print("asset identity audit failed:")
        for finding in findings:
            print(finding)
        return 1

    print(f"asset identity audit passed: {audited} identity-bearing asset(s) "
          f"with {len(sidecars)} sidecar(s), no duplicate GUID and no "
          f"case-only collision among {len(tracked)} tracked path(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
