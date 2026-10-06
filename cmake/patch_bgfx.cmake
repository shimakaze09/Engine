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
    # The uniform ring grows by inserting a chunk where its write head is.
    # The original wrapped that position modulo the chunk count, so a ring
    # of two chunks put the new one first. That shifted every chunk's place
    # in the ring, and the next frame wrote its uniforms into the chunk the
    # previous frame was still drawing from. Direct3D 12 starts with two
    # chunks and grows on the process's second frame, so that frame drew
    # with the third frame's uniforms. Upstream fixed it in bgfx 8dbdcf4
    # ("ChunkedScratchBufferT: grow the ring where the write head is.");
    # this is that fix, kept until the pin moves past it.
    engine_patch_bgfx_file(
        "${bgfx_superproject_dir}/bgfx/src/renderer.h"
        "const uint32_t chunkIndex = at % bx::max(m_chunks.size(), 1);"
        "const uint32_t chunkIndex = bx::min<uint32_t>(at, uint32_t(m_chunks.size() ) );"
        "scratch ring grows at its write head")
endfunction()
