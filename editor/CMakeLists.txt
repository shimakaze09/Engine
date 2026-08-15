# Configures editor build settings for the Engine repository.

engine_add_module_library(engine_editor
    SOURCES
    src/command_history.cpp
    src/debug_camera.cpp
    src/editor.cpp
    src/editor_commands.cpp
    src/editor_panels_assets.cpp
    src/editor_panels_diagnostics.cpp
    src/editor_panels_inspector.cpp
    src/editor_panels_main.cpp
    src/editor_panels_viewport.cpp
    src/editor_session.cpp
    src/editor_camera.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/renderer/include
    PRIVATE_INCLUDE_DIRS
    ${ENGINE_STB_INCLUDE_DIR}
    PUBLIC_DEPS
    engine_imgui
    engine_core
    engine_math
    engine_runtime
    engine_renderer
    PRIVATE_DEPS
    # editor_session.cpp/.h and editor_panels_assets.cpp call GL directly
    # for ImGui's OpenGL3 backend and thumbnail texture upload; this used to
    # arrive only transitively through engine_renderer's (formerly PUBLIC)
    # OpenGL::GL dependency, so tightening that to PRIVATE there means this
    # module needs its own explicit link (audit #178).
    OpenGL::GL
)
