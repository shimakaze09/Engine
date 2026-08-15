# Configures renderer build settings for the Engine repository.

engine_add_module_library(engine_renderer
    SOURCES
    src/asset_database.cpp
    src/asset_manager.cpp
    src/asset_stale_check.cpp
    src/asset_streaming.cpp
    src/command_buffer_builder.cpp
    src/command_buffer_capture.cpp
    src/command_buffer_context.cpp
    src/command_buffer_flush.cpp
    src/command_buffer_flush_captures.cpp
    src/command_buffer_flush_deferred.cpp
    src/command_buffer_flush_forward.cpp
    src/command_buffer_flush_post.cpp
    src/command_buffer_flush_shadows.cpp
    src/command_buffer_flush_uniforms.cpp
    src/command_buffer_ibl.cpp
    src/command_buffer_math.cpp
    src/command_buffer_post_resources.cpp
    src/command_buffer_skinning.cpp
    src/command_buffer_sky.cpp
    src/command_buffer.cpp
    src/command_buffer_init_core.cpp
    src/command_buffer_init_environment.cpp
    src/command_buffer_init_lighting.cpp
    src/command_buffer_init_post.cpp
    src/gpu_profiler.cpp
    src/light_culling.cpp
    src/lru_cache.cpp
    src/mesh_loader.cpp
    src/material_loader.cpp
    src/mesh_primitives.cpp
    src/pass_resources.cpp
    src/post_process_stack.cpp
    src/render_settings.cpp
    src/render_device_gl.cpp
    src/shader_system.cpp
    src/shadow_map.cpp
    src/texture_loader.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE_INCLUDE_DIRS
    ${ENGINE_STB_INCLUDE_DIR}
    PUBLIC_DEPS
    engine_core
    engine_math
    PRIVATE_DEPS
    # GL stays inside renderer impl (CLAUDE.md); no renderer public header
    # includes a GL type, so OpenGL::GL only needs to reach this target's
    # own .cpp files, not every consumer's link line (audit #178 — this was
    # PUBLIC, so every engine_renderer consumer pulled in an OpenGL usage
    # requirement it never directly needed; editor/CMakeLists.txt now links
    # OpenGL::GL itself since it is the one other module with real, if
    # pre-existing and separately tracked, direct GL calls).
    OpenGL::GL
    PCH
    src/pch.h
)

# Precompiled header remains PRIVATE via engine_add_module_library(... PCH ...).
