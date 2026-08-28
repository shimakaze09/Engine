# Configures runtime build settings for the Engine repository.

engine_add_module_library(engine_runtime
    SOURCES
    src/camera_manager.cpp
    src/spring_arm_update.cpp
    src/camera_component_update.cpp
    src/editor_bridge.cpp
    src/engine.cpp
    src/engine_config_strings.cpp
    src/animation.cpp
    src/animation_assets.cpp
    src/animation_system.cpp
    src/frame_pacing.cpp
    src/engine_pipeline.cpp
    src/engine_bootstrap_content.cpp
    src/engine_frame_collect.cpp
    src/engine_runtime_streaming.cpp
    src/physics_bridge.cpp
    src/prefab_serializer.cpp
    src/primitive_collider.cpp
    src/render_prep_pipeline.cpp
    src/save_data.cpp
    src/reflect_types.cpp
    src/scene_serializer.cpp
    src/scripting_bridge.cpp
    src/serialization_util.cpp
    src/service_registry.cpp
    src/timer_manager.cpp
    src/world.cpp
    src/world_components.cpp
    src/world_lifecycle.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_DEPS
    engine_core
    engine_math
    # editor_bridge.h and service_registry.h include engine/content/
    # headers, so content is part of runtime's public surface in its own
    # right, not something to reach through engine_renderer.
    engine_content
    engine_physics
    engine_scripting
    engine_renderer
    engine_audio
    ${ENGINE_SDL_TARGET}
    PCH src/pch.h
)
