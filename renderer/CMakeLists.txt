# Configures renderer build settings for the Engine repository.

# bgfx is the only render backend (#138; the legacy GL device was
# deleted at parity): the device TU, its program/uniform path, and the
# pure translation TU. The null backend stays available for headless.
engine_add_module_library(engine_renderer
    SOURCES
    src/render_device_bgfx.cpp
    src/render_device_bgfx_programs.cpp
    src/render_device_bgfx_translate.cpp
    src/asset_database.cpp
    src/asset_manager.cpp
    src/command_buffer_builder.cpp
    src/command_buffer_capture.cpp
    src/command_buffer_context.cpp
    src/dynamic_resolution.cpp
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
    src/mesh_loader.cpp
    src/material_loader.cpp
    src/material_writer.cpp
    src/mesh_primitives.cpp
    src/pass_resources.cpp
    src/post_process_stack.cpp
    src/render_settings.cpp
    src/render_device_null.cpp
    src/shader_system.cpp
    src/shadow_map.cpp
    src/texture_loader.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE_INCLUDE_DIRS
    ${ENGINE_STB_INCLUDE_DIR}
    PUBLIC_DEPS
    engine_content
    engine_core
    engine_math
    PRIVATE_DEPS
    bgfx
    bx
    bimg
    PCH
    src/pch.h
)

# Precompiled header remains PRIVATE via engine_add_module_library(... PCH ...).


