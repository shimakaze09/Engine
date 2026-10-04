# Applies the engine's local fixes to the fetched bgfx source at configure
# time, before any of it compiles. Each fix is a literal replacement that is
# idempotent: an already-patched tree is left alone, and a tree holding
# neither the original nor the patched text stops the configure, because
# a bgfx update changed the code the fix targets and it must be re-checked.
# Decision 0022 records why the engine carries these and when to drop them.

# Replaces `original` with `patched` in `file`, or confirms it is already
# patched. Each text is matched within one line, so the fix applies to an
# LF and a CRLF checkout alike, and must occur exactly once.
function(engine_patch_bgfx_file file original patched description)
    file(READ "${file}" _content)
    string(FIND "${_content}" "${patched}" _patchedAt)
    string(FIND "${_content}" "${patched}" _patchedLastAt REVERSE)
    if((NOT _patchedAt EQUAL -1) AND (_patchedAt EQUAL _patchedLastAt))
        return()
    endif()
    string(FIND "${_content}" "${original}" _originalAt)
    string(FIND "${_content}" "${original}" _originalLastAt REVERSE)
    if((_originalAt EQUAL -1) OR (NOT _originalAt EQUAL _originalLastAt)
       OR (NOT _patchedAt EQUAL -1))
        message(FATAL_ERROR
            "bgfx patch '${description}' no longer applies to ${file}: the "
            "code it fixes changed. Re-check it against the new bgfx "
            "(docs/decisions/0022-the-engine-patches-bgfx-locally.md).")
    endif()
    string(REPLACE "${original}" "${patched}" _content "${_content}")
    file(WRITE "${file}" "${_content}")
    message(STATUS "bgfx patched: ${description}")
endfunction()

# Patches the bgfx tree under `bgfx_superproject_dir` (the bgfx.cmake
# checkout, whose bgfx/ directory holds bgfx itself).
function(engine_patch_bgfx bgfx_superproject_dir)
    # Direct3D 12: the command queue's fence is created at 0 and the first
    # command list signalled 0 too, a value the fence already holds, so that
    # list counted as complete at once and its upload buffers were released
    # before the GPU ran it; the next frame then read freed memory. Starting
    # the count at 1 makes 0 mean "nothing to wait for" only.
    engine_patch_bgfx_file(
        "${bgfx_superproject_dir}/bgfx/src/renderer_d3d12.cpp"
        "m_currentFence   = 0;"
        "m_currentFence   = 1;"
        "Direct3D 12 first command list signals fence 1")
endfunction()
