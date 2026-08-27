# Configures editor build settings for the Engine repository.

engine_add_module_library(engine_editor
    SOURCES
    src/command_history.cpp
    src/debug_camera.cpp
    src/editor.cpp
    src/editor_asset_index.cpp
    src/editor_commands.cpp
    src/editor_console_capture.cpp
    src/editor_component_registry.cpp
    src/editor_inspector_metadata.cpp
    src/editor_layout.cpp
    src/editor_live_edit.cpp
    src/editor_material_edit.cpp
    src/editor_multi_edit.cpp
    src/editor_reference_pickers.cpp
    src/editor_panels_assets.cpp
    src/editor_panels_console.cpp
    src/editor_panels_diagnostics.cpp
    src/editor_panels_inspector.cpp
    src/editor_panels_inspector_generic.cpp
    src/editor_panels_inspector_custom.cpp
    src/editor_panels_main.cpp
    src/editor_panels_material.cpp
    src/editor_panels_viewport.cpp
    src/editor_scene_document.cpp
    src/editor_session.cpp
    src/editor_camera.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
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
# The editor's ImGui renderer is the bgfx one (imgui_impl_bgfx.cpp,
# using bgfx's embedded ocornut-imgui shaders) — bgfx is the engine's
# only render backend (#138).
target_sources(engine_editor PRIVATE src/imgui_impl_bgfx.cpp)
target_link_libraries(engine_editor PRIVATE bgfx bx bimg)
target_include_directories(engine_editor PRIVATE
    # The parent dir, not common/imgui itself: that dir carries a
    # wrapper imgui.h that would shadow the real one.
    "${bgfx_SOURCE_DIR}/bgfx/examples/common")
