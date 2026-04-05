add_library(engine_renderer STATIC
    src/asset_database.cpp
    src/asset_manager.cpp
    src/command_buffer.cpp
    src/mesh_loader.cpp
    src/render_device_gl.cpp
)

target_include_directories(engine_renderer
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(engine_renderer PUBLIC cxx_std_20)

target_link_libraries(engine_renderer
    PUBLIC
    engine_core
    engine_math
    OpenGL::GL
)
