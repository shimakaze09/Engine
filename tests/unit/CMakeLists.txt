add_executable(engine_unit_foundation
    smoke_test.cpp
)

target_compile_features(engine_unit_foundation PRIVATE cxx_std_20)

target_link_libraries(engine_unit_foundation
    PRIVATE
    engine_core
    engine_math
)

add_test(
    NAME engine_unit_foundation
    COMMAND engine_unit_foundation
)

add_executable(engine_unit_math
    math_test.cpp
)

target_compile_features(engine_unit_math PRIVATE cxx_std_20)

target_link_libraries(engine_unit_math
    PRIVATE
    engine_math
)

add_test(
    NAME engine_unit_math
    COMMAND engine_unit_math
)

add_executable(engine_unit_mesh_loader
    mesh_loader_test.cpp
)

target_compile_features(engine_unit_mesh_loader PRIVATE cxx_std_20)

target_link_libraries(engine_unit_mesh_loader
    PRIVATE
    engine_renderer
)

add_test(
    NAME engine_unit_mesh_loader
    COMMAND engine_unit_mesh_loader
)

add_executable(engine_unit_asset_database
    asset_database_test.cpp
)

target_compile_features(engine_unit_asset_database PRIVATE cxx_std_20)

target_link_libraries(engine_unit_asset_database
    PRIVATE
    engine_renderer
)

add_test(
    NAME engine_unit_asset_database
    COMMAND engine_unit_asset_database
)

add_executable(engine_unit_asset_manager
    asset_manager_test.cpp
)

target_compile_features(engine_unit_asset_manager PRIVATE cxx_std_20)

target_link_libraries(engine_unit_asset_manager
    PRIVATE
    engine_renderer
)

add_test(
    NAME engine_unit_asset_manager
    COMMAND engine_unit_asset_manager
)

add_executable(engine_unit_physics
    physics_test.cpp
)

target_compile_features(engine_unit_physics PRIVATE cxx_std_20)

target_link_libraries(engine_unit_physics
    PRIVATE
    engine_physics
    engine_runtime
)

add_test(
    NAME engine_unit_physics
    COMMAND engine_unit_physics
)

add_executable(engine_unit_reflect
    reflect_test.cpp
)

target_compile_features(engine_unit_reflect PRIVATE cxx_std_20)

target_link_libraries(engine_unit_reflect
    PRIVATE
    engine_core
    engine_runtime
)

add_test(
    NAME engine_unit_reflect
    COMMAND engine_unit_reflect
)

add_executable(engine_unit_scripting
    scripting_test.cpp
)

target_compile_features(engine_unit_scripting PRIVATE cxx_std_20)

target_link_libraries(engine_unit_scripting
    PRIVATE
    engine_scripting
    engine_runtime
)

add_test(
    NAME engine_unit_scripting
    COMMAND engine_unit_scripting
)

add_executable(engine_unit_scene_serializer
    scene_serializer_test.cpp
)

target_compile_features(engine_unit_scene_serializer PRIVATE cxx_std_20)

target_link_libraries(engine_unit_scene_serializer
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_unit_scene_serializer
    COMMAND engine_unit_scene_serializer
)

add_executable(engine_unit_runtime_world
    runtime_world_test.cpp
)

target_compile_features(engine_unit_runtime_world PRIVATE cxx_std_20)

target_link_libraries(engine_unit_runtime_world
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_unit_runtime_world
    COMMAND engine_unit_runtime_world
)

add_executable(engine_unit_vfs
    vfs_test.cpp
)

target_compile_features(engine_unit_vfs PRIVATE cxx_std_20)

target_link_libraries(engine_unit_vfs
    PRIVATE
    engine_core
)

add_test(
    NAME engine_unit_vfs
    COMMAND engine_unit_vfs
)

add_executable(engine_unit_event_bus
    event_bus_test.cpp
)

target_compile_features(engine_unit_event_bus PRIVATE cxx_std_20)

target_link_libraries(engine_unit_event_bus
    PRIVATE
    engine_core
)

add_test(
    NAME engine_unit_event_bus
    COMMAND engine_unit_event_bus
)

add_executable(engine_unit_input
    input_test.cpp
)

target_compile_features(engine_unit_input PRIVATE cxx_std_20)

target_link_libraries(engine_unit_input
    PRIVATE
    engine_core
)

add_test(
    NAME engine_unit_input
    COMMAND engine_unit_input
)

add_executable(engine_unit_profiler
    profiler_test.cpp
)

target_compile_features(engine_unit_profiler PRIVATE cxx_std_20)

target_link_libraries(engine_unit_profiler
    PRIVATE
    engine_core
)

add_test(
    NAME engine_unit_profiler
    COMMAND engine_unit_profiler
)

add_executable(engine_unit_shader_system
    shader_system_test.cpp
)

target_compile_features(engine_unit_shader_system PRIVATE cxx_std_20)

target_link_libraries(engine_unit_shader_system
    PRIVATE
    engine_renderer
)

add_test(
    NAME engine_unit_shader_system
    COMMAND engine_unit_shader_system
)

add_executable(engine_unit_audio
    audio_test.cpp
)

target_compile_features(engine_unit_audio PRIVATE cxx_std_20)

target_link_libraries(engine_unit_audio
    PRIVATE
    engine_audio
)

add_test(
    NAME engine_unit_audio
    COMMAND engine_unit_audio
)

add_executable(engine_unit_texture_loader
    texture_loader_test.cpp
)

target_compile_features(engine_unit_texture_loader PRIVATE cxx_std_20)

target_link_libraries(engine_unit_texture_loader
    PRIVATE
    engine_renderer
)

add_test(
    NAME engine_unit_texture_loader
    COMMAND engine_unit_texture_loader
)

add_executable(engine_unit_prefab
    prefab_test.cpp
)

target_compile_features(engine_unit_prefab PRIVATE cxx_std_20)

target_link_libraries(engine_unit_prefab
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_unit_prefab
    COMMAND engine_unit_prefab
)
