# Configures the content validation tool: catalogues the engine's and the
# project's content, loads each scene, prefab, material and animation
# controller through its production loader and reports its validation
# findings for CI.

engine_add_executable_target(engine_validate
    SOURCES
    main.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_renderer
    engine_content
    engine_core
)
