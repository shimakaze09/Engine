# Configures editor build settings for the Engine repository.

engine_add_module_library(engine_editor
    SOURCES
    src/command_history.cpp
    src/debug_camera.cpp
    src/editor.cpp
    src/editor_commands.cpp
    src/editor_console_capture.cpp
    src/editor_component_registry.cpp
    src/editor_inspector_metadata.cpp
    src/editor_reference_pickers.cpp
    src/editor_panels_assets.cpp
    src/editor_panels_console.cpp
    src/editor_panels_diagnostics.cpp
    src/editor_panels_inspector.cpp
    src/editor_panels_inspector_generic.cpp
    src/editor_panels_inspector_custom.cpp
    src/editor_panels_main.cpp
    src/editor_panels_viewport.cpp
    src/editor_scene_document.cpp
    src/editor_session.cpp
    src/editor_camera.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/renderer/include
    PRIVATE_INCLUDE_DIRS
    ${ENGINE_STB_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/runtime/src
    PUBLIC_DEPS
    engine_imgui
    engine_core
    engine_math
    engine_runtime
    engine_renderer
)
# No PRIVATE_DEPS OpenGL::GL (audit #206 closure of the #178 note above it):
# editor_session.cpp/.h and editor_panels_assets.cpp no longer call GL
# directly -- thumbnail upload routes through engine_renderer's RenderDevice,
# and the ImGui OpenGL3 backend links its own GL symbols through
# engine_imgui, which this module already depends on publicly.
