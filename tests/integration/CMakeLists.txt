# Verifies tests behavior for the Engine test suite.

engine_add_test_executable(engine_integration_ecs
    SOURCES
    smoke_integration.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    gpu;integration
)

engine_add_test_executable(engine_integration_pipeline_camera_prep
    SOURCES
    pipeline_camera_prep_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    gpu;integration
)

engine_add_test_executable(engine_integration_vertical_slice
    SOURCES
    vertical_slice_integration.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_scheduler_stress
    SOURCES
    scheduler_stress.cpp
    PRIVATE_DEPS
    engine_core
    LABELS
    gpu;integration
)

engine_add_test_executable(engine_integration_thread_count_determinism
    SOURCES
    thread_count_determinism.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_determinism
    SOURCES
    determinism_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_ecs_stress_50k
    SOURCES
    ecs_stress_50k.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_lifecycle
    SOURCES
    lifecycle_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_scene_flow
    SOURCES
    scene_flow_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    engine_renderer
    engine_core
    LABELS
    integration
)

engine_add_test_executable(engine_integration_island_template
    SOURCES
    island_template_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_lua_lifecycle
    SOURCES
    lua_lifecycle_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_script_begin_play_retry
    SOURCES
    script_begin_play_retry_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_script_deferred_write
    SOURCES
    script_deferred_write_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_hotreload_rollback
    SOURCES
    hotreload_rollback_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_script_module_cache
    SOURCES
    script_module_cache_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_script_path_ingress
    SOURCES
    script_path_ingress_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_game_mode
    SOURCES
    game_mode_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_timer
    SOURCES
    timer_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_camera
    SOURCES
    camera_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_coroutine
    SOURCES
    coroutine_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_sandbox
    SOURCES
    sandbox_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_lua_hardening
    SOURCES
    lua_hardening_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

# ---- P1-M4-C / P1-M4-D: Async Streaming & LRU ----

engine_add_test_executable(engine_integration_async_load
    SOURCES
    async_load_test.cpp
    PRIVATE_DEPS
    engine_renderer
    LABELS
    integration
)

engine_add_test_executable(engine_integration_streaming_budget
    SOURCES
    streaming_budget_test.cpp
    PRIVATE_DEPS
    engine_renderer
    LABELS
    integration
)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME engine_integration_tool_gates
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_tool_gates.py
    )
    set_tests_properties(
        engine_integration_tool_gates
        PROPERTIES LABELS integration
    )
endif()

if(ENGINE_BUILD_TOOLS)
    add_test(
        NAME engine_integration_asset_packer_rejects_corrupt_texture
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DINPUT=${CMAKE_SOURCE_DIR}/tests/data/corrupt_texture.png"
            "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/corrupt_texture.asset"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_packer_corrupt_texture_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_packer_rejects_corrupt_texture
        PROPERTIES LABELS integration
    )

    add_test(
        NAME engine_integration_asset_packer_rejects_corrupt_graph
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DINPUT=${CMAKE_SOURCE_DIR}/tests/data/corrupt_texture.png"
            "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/corrupt_graph.asset"
            "-DGRAPH=${CMAKE_SOURCE_DIR}/tests/data/corrupt_dependency_graph.json"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_packer_corrupt_graph_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_packer_rejects_corrupt_graph
        PROPERTIES LABELS integration
    )

    add_test(
        NAME engine_integration_asset_packer_bin_dependency
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DSRC_GLTF=${CMAKE_SOURCE_DIR}/assets/props/coin.gltf"
            "-DSRC_BIN=${CMAKE_SOURCE_DIR}/assets/props/coin.bin"
            "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/bin_dep_work"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_packer_bin_dependency_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_packer_bin_dependency
        PROPERTIES LABELS integration
    )

    add_test(
        NAME engine_integration_asset_packer_thumbnail_settings
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DSRC_GLTF=${CMAKE_SOURCE_DIR}/assets/props/coin.gltf"
            "-DSRC_BIN=${CMAKE_SOURCE_DIR}/assets/props/coin.bin"
            "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/thumb_settings_work"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_packer_thumbnail_settings_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_packer_thumbnail_settings
        PROPERTIES LABELS integration
    )

    add_test(
        NAME engine_integration_asset_packer_stale_hull_removed
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DSRC_GLTF=${CMAKE_SOURCE_DIR}/assets/props/coin.gltf"
            "-DSRC_BIN=${CMAKE_SOURCE_DIR}/assets/props/coin.bin"
            "-DHULLLESS_GLTF=${CMAKE_SOURCE_DIR}/tests/data/hullless_triangle.gltf"
            "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/stale_hull_work"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_packer_stale_hull_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_packer_stale_hull_removed
        PROPERTIES LABELS integration
    )

    add_test(
        NAME engine_integration_asset_packer_stale_anim_removed
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DSRC_GLTF=${CMAKE_SOURCE_DIR}/assets/character.gltf"
            "-DSRC_BIN=${CMAKE_SOURCE_DIR}/assets/character.bin"
            "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/stale_anim_work"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_packer_stale_anim_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_packer_stale_anim_removed
        PROPERTIES LABELS integration
    )

    add_test(
        NAME engine_integration_asset_packer_missing_sidecar_recook
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DSRC_GLTF=${CMAKE_SOURCE_DIR}/assets/character.gltf"
            "-DSRC_BIN=${CMAKE_SOURCE_DIR}/assets/character.bin"
            "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/missing_sidecar_work"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_packer_missing_sidecar_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_packer_missing_sidecar_recook
        PROPERTIES LABELS integration
    )

    add_test(
        NAME engine_integration_asset_packer_legacy_stamp_recook
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DSRC_GLTF=${CMAKE_SOURCE_DIR}/assets/character.gltf"
            "-DSRC_BIN=${CMAKE_SOURCE_DIR}/assets/character.bin"
            "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/legacy_stamp_work"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_packer_legacy_stamp_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_packer_legacy_stamp_recook
        PROPERTIES LABELS integration
    )

    add_test(
        NAME engine_integration_asset_packer_platform_tag
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DSRC_GLTF=${CMAKE_SOURCE_DIR}/assets/props/coin.gltf"
            "-DSRC_BIN=${CMAKE_SOURCE_DIR}/assets/props/coin.bin"
            "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/platform_tag_work"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_packer_platform_tag_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_packer_platform_tag
        PROPERTIES LABELS integration
    )

    add_test(
        NAME engine_integration_asset_packer_orphan_sweep
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DSRC_GLTF=${CMAKE_SOURCE_DIR}/assets/props/coin.gltf"
            "-DSRC_BIN=${CMAKE_SOURCE_DIR}/assets/props/coin.bin"
            "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/orphan_sweep_work"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_packer_orphan_sweep_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_packer_orphan_sweep
        PROPERTIES LABELS integration
    )

    engine_add_executable_target(asset_stale_load_host
        SOURCES
        asset_stale_load_test.cpp
        PRIVATE_DEPS
        engine_renderer
    )

    add_test(
        NAME engine_integration_asset_runtime_stale_warning
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DSTALE_HOST=$<TARGET_FILE:asset_stale_load_host>"
            "-DSRC_GLTF=${CMAKE_SOURCE_DIR}/assets/props/coin.gltf"
            "-DSRC_BIN=${CMAKE_SOURCE_DIR}/assets/props/coin.bin"
            "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/runtime_stale_work"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_runtime_stale_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_runtime_stale_warning
        PROPERTIES LABELS integration
    )

    engine_add_executable_target(asset_stale_skeletal_load_host
        SOURCES
        asset_stale_skeletal_load_test.cpp
        PRIVATE_DEPS
        engine_runtime
    )

    add_test(
        NAME engine_integration_asset_runtime_stale_warning_skeletal
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_PACKER=$<TARGET_FILE:asset_packer>"
            "-DSTALE_HOST=$<TARGET_FILE:asset_stale_skeletal_load_host>"
            "-DSRC_GLTF=${CMAKE_SOURCE_DIR}/assets/character.gltf"
            "-DSRC_BIN=${CMAKE_SOURCE_DIR}/assets/character.bin"
            "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/runtime_stale_skeletal_work"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/asset_runtime_stale_skeletal_test.cmake
    )
    set_tests_properties(
        engine_integration_asset_runtime_stale_warning_skeletal
        PROPERTIES LABELS integration
    )
endif()

engine_add_test_executable(engine_integration_hotreload
    SOURCES
    hotreload_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_bindgen
    SOURCES
    bindgen_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_dap
    SOURCES
    dap_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

if(WIN32)
    target_link_libraries(engine_integration_dap PRIVATE ws2_32)
endif()

# ---- P1-M4-B: Dependency Graph ----

engine_add_test_executable(engine_integration_asset_dep_load
    SOURCES
    asset_dep_load_test.cpp
    PRIVATE_DEPS
    engine_renderer
    LABELS
    integration
)

# ---- H-16 (#85): scene-transition reset protocol ----

engine_add_test_executable(engine_integration_scene_transition_reset
    SOURCES
    scene_transition_reset_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    engine_renderer
    engine_core
    LABELS
    integration
)

# ---- #120: injected frame-stage fatal must map to FatalFrame ----

engine_add_test_executable(engine_integration_run_fatal_frame
    SOURCES
    run_fatal_frame_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_core
    LABELS
    gpu;integration
)
