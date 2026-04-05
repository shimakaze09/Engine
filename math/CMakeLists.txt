add_library(engine_math STATIC
    src/mat4.cpp
    src/quat.cpp
    src/transform.cpp
    src/vec2.cpp
    src/vec3.cpp
    src/vec4.cpp
)

target_include_directories(engine_math
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(engine_math PUBLIC cxx_std_20)

target_link_libraries(engine_math
    PUBLIC
        engine_core
)
