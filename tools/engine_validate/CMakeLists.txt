# Configures the scene validation tool: loads each scene through the
# production loader and reports its validation findings for CI.

engine_add_executable_target(engine_validate
    SOURCES
    main.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_core
)
