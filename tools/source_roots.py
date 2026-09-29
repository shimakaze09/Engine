"""The first-party engine source roots, named once for every source gate.

Each top-level directory holding engine or application code is listed
here, so a new one (the player, the project hub) is audited by every gate
the moment it is added rather than by whichever gates someone remembers
to update. A gate adds the trees it treats differently: `tests`, `tools`.
tools/check_module_deps.py keeps its own table, which is a dependency
graph rather than a list.
"""

ENGINE_SOURCE_ROOTS: tuple[str, ...] = (
    "app",
    "audio",
    "content",
    "core",
    "editor",
    "math",
    "physics",
    "player",
    "renderer",
    "runtime",
    "scripting",
)
