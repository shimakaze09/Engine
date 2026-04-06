add_library(engine_editor STATIC
    src/command_history.cpp
    src/editor.cpp
    src/editor_camera.cpp
)

target_include_directories(engine_editor
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/renderer/include
)

target_compile_features(engine_editor PUBLIC cxx_std_20)

target_link_libraries(engine_editor
    PUBLIC
    engine_imgui
    engine_core
    engine_math
    engine_runtime
    engine_renderer
)
