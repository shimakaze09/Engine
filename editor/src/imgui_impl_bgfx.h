// Declares the editor's ImGui renderer: the stock ImGui
// backend Init/Shutdown/NewFrame/RenderDrawData quartet, submitting
// ImGui draw data through bgfx into a dedicated late view. The editor
// links bgfx directly here — the sanctioned UI-integration exception to
// keeping backend types inside the renderer.

#pragma once

/// Creates the ImGui program and vertex layout and declares that this
/// renderer honours ImGui's texture requests; the font atlas is created on
/// the first frame that draws.
bool ImGui_ImplBgfx_Init();
/// Destroys every bgfx resource the backend created.
void ImGui_ImplBgfx_Shutdown();
/// Per-frame hook (kept for surface parity; no bgfx work needed).
void ImGui_ImplBgfx_NewFrame();
/// Submits the draw data into the editor's bgfx view.
void ImGui_ImplBgfx_RenderDrawData(struct ImDrawData *drawData);
