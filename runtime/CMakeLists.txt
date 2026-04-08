add_library(engine_runtime STATIC
    src/editor_bridge.cpp
    src/engine.cpp
    src/prefab_serializer.cpp
    src/render_prep_pipeline.cpp
    src/reflect_types.cpp
    src/scene_serializer.cpp
    src/scripting_bridge.cpp
    src/world.cpp
)

target_include_directories(engine_runtime
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(engine_runtime PUBLIC cxx_std_20)

target_link_libraries(engine_runtime
    PUBLIC
    engine_core
    engine_math
    engine_physics
    engine_scripting
    engine_renderer
    engine_audio
    ${ENGINE_SDL2_TARGET}
    OpenGL::GL
)
