// Declares the editor's ImGui renderer for the bgfx backend (#138):
// the Init/Shutdown/NewFrame/RenderDrawData quartet mirroring the stock
// imgui_impl_opengl3 surface, submitting ImGui draw data through bgfx
// into a dedicated late view. Compiled only in bgfx builds; the editor
// links the backend directly here, the same sanctioned pattern as its
// direct GL calls under the gl backend.

#pragma once

/// Creates the font atlas texture, ImGui program, and vertex layout.
bool ImGui_ImplBgfx_Init();
/// Destroys every bgfx resource the backend created.
void ImGui_ImplBgfx_Shutdown();
/// Per-frame hook (kept for surface parity; no bgfx work needed).
void ImGui_ImplBgfx_NewFrame();
/// Submits the draw data into the editor's bgfx view.
void ImGui_ImplBgfx_RenderDrawData(struct ImDrawData *drawData);
