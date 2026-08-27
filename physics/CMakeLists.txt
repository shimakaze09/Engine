# Configures physics build settings for the Engine repository.

engine_add_module_library(engine_physics
    SOURCES
    src/physics.cpp
    src/physics_payloads.cpp
    src/physics_step.cpp
    src/blocked_body_diagnostic.cpp
    src/collider.cpp
    src/contact_clip.cpp
    src/contact_resolution.cpp
    src/convex_hull.cpp
    src/ccd.cpp
    src/constraint_solver.cpp
    src/contact_manifold.cpp
    src/narrow_phase.cpp
    src/physics_query.cpp
    src/primitive_hulls.cpp
    src/joints/distance_joint.cpp
    src/joints/hinge_joint.cpp
    src/joints/ball_joint.cpp
    src/joints/slider_joint.cpp
    src/joints/spring_joint.cpp
    src/joints/fixed_joint.cpp
    src/joints/joint_projection.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_DEPS
    engine_math
    # PUBLIC, not PRIVATE: physics_context.h and physics_types.h include
    # engine/core/entity.h, so a consumer of physics headers needs core's
    # include dirs from physics itself rather than through engine_math.
    engine_core
    PCH src/pch.h
)
