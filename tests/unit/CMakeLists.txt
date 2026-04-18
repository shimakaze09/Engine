engine_add_test_executable(engine_unit_foundation
    SOURCES
    smoke_test.cpp
    PRIVATE_DEPS
    engine_core
    engine_math
    LABELS
    gpu
)

engine_add_test_executable(engine_unit_math
    SOURCES
    math_test.cpp
    PRIVATE_DEPS
    engine_math
)

engine_add_test_executable(engine_unit_mesh_loader
    SOURCES
    mesh_loader_test.cpp
    PRIVATE_DEPS
    engine_renderer
)

engine_add_test_executable(engine_unit_asset_database
    SOURCES
    asset_database_test.cpp
    PRIVATE_DEPS
    engine_renderer
)

engine_add_test_executable(engine_unit_asset_manager
    SOURCES
    asset_manager_test.cpp
    PRIVATE_DEPS
    engine_renderer
)

engine_add_test_executable(engine_unit_physics
    SOURCES
    physics_test.cpp
    PRIVATE_DEPS
    engine_physics
    engine_runtime
)

engine_add_test_executable(engine_unit_reflect
    SOURCES
    reflect_test.cpp
    PRIVATE_DEPS
    engine_core
    engine_runtime
)

engine_add_test_executable(engine_unit_scripting
    SOURCES
    scripting_test.cpp
    PRIVATE_DEPS
    engine_scripting
    engine_runtime
)

engine_add_test_executable(engine_unit_scene_serializer
    SOURCES
    scene_serializer_test.cpp
    PRIVATE_DEPS
    engine_runtime
)

engine_add_test_executable(engine_unit_runtime_world
    SOURCES
    runtime_world_test.cpp
    PRIVATE_DEPS
    engine_runtime
)

engine_add_test_executable(engine_unit_vfs
    SOURCES
    vfs_test.cpp
    PRIVATE_DEPS
    engine_core
)

engine_add_test_executable(engine_unit_event_bus
    SOURCES
    event_bus_test.cpp
    PRIVATE_DEPS
    engine_core
)

engine_add_test_executable(engine_unit_input
    SOURCES
    input_test.cpp
    PRIVATE_DEPS
    engine_core
)

engine_add_test_executable(engine_unit_profiler
    SOURCES
    profiler_test.cpp
    PRIVATE_DEPS
    engine_core
)

engine_add_test_executable(engine_unit_pool_allocator
    SOURCES
    pool_allocator_test.cpp
    PRIVATE_DEPS
    engine_core
)

engine_add_test_executable(engine_unit_engine_stats
    SOURCES
    engine_stats_test.cpp
    PRIVATE_DEPS
    engine_core
)

engine_add_test_executable(engine_unit_gpu_profiler
    SOURCES
    gpu_profiler_test.cpp
    PRIVATE_DEPS
    engine_renderer
)

engine_add_test_executable(engine_unit_shader_system
    SOURCES
    shader_system_test.cpp
    PRIVATE_DEPS
    engine_renderer
)

engine_add_test_executable(engine_unit_audio
    SOURCES
    audio_test.cpp
    PRIVATE_DEPS
    engine_audio
)

engine_add_test_executable(engine_unit_texture_loader
    SOURCES
    texture_loader_test.cpp
    PRIVATE_DEPS
    engine_renderer
)

engine_add_test_executable(engine_unit_prefab
    SOURCES
    prefab_test.cpp
    PRIVATE_DEPS
    engine_runtime
)

# ---- P1-M1: Engine Production Baseline ----

engine_add_test_executable(engine_unit_cvar_console
    SOURCES
    cvar_console_test.cpp
    PRIVATE_DEPS
    engine_core
)

engine_add_test_executable(engine_unit_debug_draw
    SOURCES
    debug_draw_test.cpp
    PRIVATE_DEPS
    engine_core
)

engine_add_test_executable(engine_unit_ecs_stress
    SOURCES
    ecs_stress_test.cpp
    PRIVATE_DEPS
    engine_runtime
)

engine_add_test_executable(engine_unit_determinism
    SOURCES
    determinism_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_physics
)

engine_add_test_executable(engine_unit_mem_tracker
    SOURCES
    mem_tracker_test.cpp
    PRIVATE_DEPS
    engine_core
)

engine_add_test_executable(engine_unit_entity_pool
    SOURCES
    entity_pool_test.cpp
    PRIVATE_DEPS
    engine_runtime
)

engine_add_test_executable(engine_unit_service_locator
    SOURCES
    service_locator_test.cpp
    PRIVATE_DEPS
    engine_core
)

# ---- P1-M2-C: Input System ----

engine_add_test_executable(engine_unit_input_map
    SOURCES
    input_map_test.cpp
    PRIVATE_DEPS
    engine_core
)

engine_add_test_executable(engine_unit_touch_input
    SOURCES
    touch_input_test.cpp
    PRIVATE_DEPS
    engine_core
)

# ---- P1-M3: Physics Engine Hardening ----

engine_add_test_executable(engine_unit_collision_layer
    SOURCES
    collision_layer_test.cpp
    PRIVATE_DEPS
    engine_physics
    engine_runtime
)

engine_add_test_executable(engine_unit_joints
    SOURCES
    joint_test.cpp
    PRIVATE_DEPS
    engine_physics
    engine_runtime
)

engine_add_test_executable(engine_unit_manifold
    SOURCES
    manifold_test.cpp
    PRIVATE_DEPS
    engine_physics
)

engine_add_test_executable(engine_unit_physics_query
    SOURCES
    physics_query_test.cpp
    PRIVATE_DEPS
    engine_physics
    engine_runtime
)

engine_add_test_executable(engine_unit_ccd
    SOURCES
    ccd_test.cpp
    PRIVATE_DEPS
    engine_physics
    engine_runtime
)

engine_add_test_executable(engine_unit_speculative_contacts
    SOURCES
    speculative_contacts_test.cpp
    PRIVATE_DEPS
    engine_physics
    engine_runtime
)

# ---- P1-M4-A: Asset Database Hardening ----

engine_add_test_executable(engine_unit_asset_hash
    SOURCES
    asset_hash_test.cpp
    PRIVATE_DEPS
    engine_renderer
)

# ---- P1-M4-B: Dependency Graph ----

engine_add_test_executable(engine_unit_dependency_graph
    SOURCES
    dependency_graph_test.cpp
    ${CMAKE_SOURCE_DIR}/tools/asset_packer/dependency_graph.cpp
    PRIVATE_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/tools/asset_packer
)

if(WIN32)
    target_compile_definitions(engine_unit_dependency_graph PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- P1-M4-C: Async Streaming ----

engine_add_test_executable(engine_unit_async_streaming
    SOURCES
    async_streaming_test.cpp
    PRIVATE_DEPS
    engine_renderer
)

# ---- P1-M4-D: LRU Eviction ----

engine_add_test_executable(engine_unit_lru_cache
    SOURCES
    lru_cache_test.cpp
    PRIVATE_DEPS
    engine_renderer
)

# ---- P1-M4-E: Deterministic Cooking ----

engine_add_test_executable(engine_unit_deterministic_cook
    SOURCES
    deterministic_cook_test.cpp
    PRIVATE_DEPS
    engine_core
)

engine_add_test_executable(engine_unit_import_settings
    SOURCES
    import_settings_test.cpp
    PRIVATE_DEPS
    engine_core
)

# ---- P1-M5: Deferred Rendering ----

engine_add_test_executable(engine_unit_deferred_light_component
    SOURCES
    deferred_light_component_test.cpp
    PRIVATE_DEPS
    engine_runtime
)

engine_add_test_executable(engine_unit_light_culling
    SOURCES
    light_culling_test.cpp
    PRIVATE_DEPS
    engine_renderer
    engine_math
)
