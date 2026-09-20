# Shared publish primitive for the bundled-asset generators (#351): a set
# of staged files is committed by replacing each final path, then writing
# the directory's generated.manifest last. The manifest names every
# published file with its byte size and FNV-1a 64 hash and is the consumer
# commit boundary: the asset packer refuses to cook a listed source whose
# bytes disagree with it, so an interrupted publish (new files beside the
# previous manifest) can never be cooked as a mixed generation. The same
# manifest stays valid for files another generator publishes into the same
# directory: entries are merged, never dropped.
import json
import os

MANIFEST_NAME = "generated.manifest"
MANIFEST_SCHEMA = 1

# Test hook: after this many replacements the publish raises, leaving the
# previous manifest in place beside a partly replaced set.
FAIL_AFTER_ENV = "ENGINE_GEN_FAIL_AFTER_REPLACE"

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
FNV_MASK = 0xFFFFFFFFFFFFFFFF


def fnv1a64(data):
    """FNV-1a 64 over bytes, matching engine::core::fnv1a_64_append."""
    value = FNV_OFFSET
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & FNV_MASK
    return value


def hash_file(path):
    with open(path, "rb") as f:
        return fnv1a64(f.read())


def manifest_path_for(directory):
    return os.path.join(directory, MANIFEST_NAME)


def load_manifest(directory):
    """Returns the directory's manifest entries ({} when absent)."""
    path = manifest_path_for(directory)
    if not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8") as f:
        document = json.load(f)
    if document.get("schema") != MANIFEST_SCHEMA:
        raise ValueError(f"unknown manifest schema in {path}")
    return dict(document.get("files", {}))


def write_manifest(directory, entries):
    """Atomically writes the manifest (staged sibling, then replace)."""
    path = manifest_path_for(directory)
    document = {"schema": MANIFEST_SCHEMA,
                "files": {name: entries[name] for name in sorted(entries)}}
    with open(path + ".tmp", "w", encoding="utf-8", newline="\n") as f:
        json.dump(document, f, indent=1, sort_keys=True)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(path + ".tmp", path)


def publish_set(staged):
    """Commits (tmp_path, final_path) pairs, then the manifest.

    Every final path must live in one directory. The manifest entry of each
    published file is computed from its staged bytes before any replacement,
    so the manifest written last describes exactly the set that was
    published. Raises before the manifest write after
    ENGINE_GEN_FAIL_AFTER_REPLACE replacements (test hook).
    """
    if not staged:
        return
    directories = {os.path.dirname(os.path.abspath(final))
                   for _, final in staged}
    if len(directories) != 1:
        raise ValueError("a published set must live in one directory")
    directory = directories.pop()
    entries = load_manifest(directory)
    for tmp_path, final_path in staged:
        with open(tmp_path, "rb") as f:
            data = f.read()
        entries[os.path.basename(final_path)] = {
            "bytes": len(data), "fnv1a64": f"{fnv1a64(data):016x}"}
    fail_after = os.environ.get(FAIL_AFTER_ENV)
    fail_after = int(fail_after) if fail_after else None
    for index, (tmp_path, final_path) in enumerate(staged):
        if fail_after is not None and index >= fail_after:
            raise RuntimeError(
                f"injected publish failure after {index} replacements")
        os.replace(tmp_path, final_path)
    write_manifest(directory, entries)
