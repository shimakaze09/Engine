add_library(engine_physics STATIC
    src/physics.cpp
    src/convex_hull.cpp
    src/constraint_solver.cpp
    src/contact_manifold.cpp
    src/physics_query.cpp
    src/joints/distance_joint.cpp
    src/joints/hinge_joint.cpp
    src/joints/ball_joint.cpp
    src/joints/slider_joint.cpp
    src/joints/spring_joint.cpp
    src/joints/fixed_joint.cpp
)

target_include_directories(engine_physics
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/core/include
)

target_compile_features(engine_physics PUBLIC cxx_std_20)

target_link_libraries(engine_physics
    PUBLIC
    engine_math
    PRIVATE
    engine_core
)
