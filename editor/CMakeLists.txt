# Configures editor build settings for the Engine repository.

engine_add_module_library(engine_editor
    SOURCES
    src/command_history.cpp
    src/camera_frustum_overlay.cpp
    src/editor.cpp
    src/editor_asset_index.cpp
    src/editor_asset_create.cpp
    src/editor_autosave.cpp
    src/editor_asset_labels.cpp
    src/editor_asset_usages.cpp
    src/editor_import_settings.cpp
    src/editor_commands.cpp
    src/editor_component_ops.cpp
    src/editor_entity_commands.cpp
    src/editor_asset_place.cpp
    src/editor_entity_rename.cpp
    src/editor_entity_tags.cpp
    src/editor_script_behaviours.cpp
    src/editor_script_properties.cpp
    src/editor_entity_menus.cpp
    src/editor_entity_clipboard.cpp
    src/editor_frame_history.cpp
    src/editor_frame_selection.cpp
    src/editor_grid.cpp
    src/editor_light_gizmos.cpp
    src/editor_scene_query.cpp
    src/editor_view_drag.cpp
    src/editor_console_capture.cpp
    src/editor_console_commands.cpp
    src/editor_screenshot.cpp
    src/editor_project_files.cpp
    src/editor_play_recording.cpp
    src/editor_fonts.cpp
    src/editor_component_registry.cpp
    src/editor_inspector_metadata.cpp
    src/editor_inspector_widgets.cpp
    src/editor_layout.cpp
    src/editor_live_edit.cpp
    src/editor_material_edit.cpp
    src/editor_multi_edit.cpp
    src/editor_reference_pickers.cpp
    src/editor_panels_assets.cpp
    src/editor_panels_console.cpp
    src/editor_panels_rendering.cpp
    src/editor_preferences.cpp
    src/editor_project_settings.cpp
    src/editor_panels_diagnostics.cpp
    src/editor_panels_inspector.cpp
    src/editor_panels_inspector_generic.cpp
    src/editor_panels_inspector_custom.cpp
    src/editor_nav_mesh_bake.cpp
    src/editor_panels_main.cpp
    src/editor_panels_material.cpp
    src/editor_panels_viewport.cpp
    src/editor_scene_document.cpp
    src/editor_recovery.cpp
    src/editor_recent_list.cpp
    src/editor_project_hub.cpp
    src/editor_session.cpp
    src/editor_shortcuts.cpp
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
# only render backend.
target_sources(engine_editor PRIVATE src/imgui_impl_bgfx.cpp)
target_link_libraries(engine_editor PRIVATE bgfx bx bimg)
target_include_directories(engine_editor PRIVATE
    # The parent dir, not common/imgui itself: that dir carries a
    # wrapper imgui.h that would shadow the real one.
    "${bgfx_SOURCE_DIR}/bgfx/examples/common")
