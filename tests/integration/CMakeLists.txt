# Verifies tests behavior for the Engine test suite.

engine_add_test_executable(engine_integration_ecs
    SOURCES
    smoke_integration.cpp
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

engine_add_test_executable(engine_integration_lua_lifecycle
    SOURCES
    lua_lifecycle_test.cpp
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
