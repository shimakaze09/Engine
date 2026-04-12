add_library(engine_physics STATIC
    src/physics.cpp
    src/convex_hull.cpp
)

target_include_directories(engine_physics
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/include
)

target_compile_features(engine_physics PUBLIC cxx_std_20)

target_link_libraries(engine_physics
    PUBLIC
    engine_math
)
