// Implements the editor's ImGui renderer for the bgfx backend (#138),
// adapted from bgfx's examples/common/imgui renderer (BSD-2, Branimir
// Karadzic): transient vertex/index buffers per draw list, per-command
// scissors, alpha blending, and the embedded precompiled ocornut-imgui
// shaders. Draws submit into a fixed late view (255) so the UI renders
// after every engine pass regardless of the frame's view allocation.
// ImTextureID carries RenderDevice::native_texture_id values, which the
// bgfx backend defines as the bgfx texture handle index.

#include "imgui_impl_bgfx.h"

#include <imgui.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>
#include <imgui/vs_ocornut_imgui.bin.h>
#include <imgui/fs_ocornut_imgui.bin.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cstdint>
#include <cstring>

namespace {

constexpr bgfx::ViewId kImGuiViewId = 255;

const bgfx::EmbeddedShader kEmbeddedShaders[] = {
    BGFX_EMBEDDED_SHADER(vs_ocornut_imgui),
    BGFX_EMBEDDED_SHADER(fs_ocornut_imgui),
    BGFX_EMBEDDED_SHADER_END()};

bgfx::ProgramHandle g_program = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_sampler = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_fontTexture = BGFX_INVALID_HANDLE;
bgfx::VertexLayout g_vertexLayout{};

} // namespace

bool ImGui_ImplBgfx_Init() {
  const bgfx::RendererType::Enum type = bgfx::getRendererType();
  g_program = bgfx::createProgram(
      bgfx::createEmbeddedShader(kEmbeddedShaders, type, "vs_ocornut_imgui"),
      bgfx::createEmbeddedShader(kEmbeddedShaders, type, "fs_ocornut_imgui"),
      true);
  if (!bgfx::isValid(g_program)) {
    return false;
  }
  g_sampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);

  g_vertexLayout.begin()
      .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
      .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
      .end();

  ImGuiIO &io = ImGui::GetIO();
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  g_fontTexture = bgfx::createTexture2D(
      static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height),
      false, 1, bgfx::TextureFormat::BGRA8, 0,
      bgfx::copy(pixels, static_cast<std::uint32_t>(width) *
                             static_cast<std::uint32_t>(height) * 4U));
  if (!bgfx::isValid(g_fontTexture)) {
    ImGui_ImplBgfx_Shutdown();
    return false;
  }
  io.Fonts->SetTexID(
      static_cast<ImTextureID>(static_cast<std::uintptr_t>(g_fontTexture.idx)));
  io.BackendRendererName = "imgui_impl_bgfx";
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  return true;
}

void ImGui_ImplBgfx_Shutdown() {
  if (bgfx::isValid(g_fontTexture)) {
    bgfx::destroy(g_fontTexture);
    g_fontTexture = BGFX_INVALID_HANDLE;
  }
  if (bgfx::isValid(g_sampler)) {
    bgfx::destroy(g_sampler);
    g_sampler = BGFX_INVALID_HANDLE;
  }
  if (bgfx::isValid(g_program)) {
    bgfx::destroy(g_program);
    g_program = BGFX_INVALID_HANDLE;
  }
}

void ImGui_ImplBgfx_NewFrame() {}

void ImGui_ImplBgfx_RenderDrawData(ImDrawData *drawData) {
  if ((drawData == nullptr) || !bgfx::isValid(g_program)) {
    return;
  }
  const float width = drawData->DisplaySize.x;
  const float height = drawData->DisplaySize.y;
  if ((width <= 0.0F) || (height <= 0.0F)) {
    return;
  }

  bgfx::setViewName(kImGuiViewId, "editor-imgui");
  bgfx::setViewMode(kImGuiViewId, bgfx::ViewMode::Sequential);
  bgfx::setViewFrameBuffer(kImGuiViewId, BGFX_INVALID_HANDLE);
  bgfx::setViewRect(kImGuiViewId, 0, 0, static_cast<std::uint16_t>(width),
                    static_cast<std::uint16_t>(height));

  // Column-major ortho: x [L,R] -> [-1,1], y [T,B] -> [1,-1], z [0,1].
  const float L = drawData->DisplayPos.x;
  const float R = drawData->DisplayPos.x + width;
  const float T = drawData->DisplayPos.y;
  const float B = drawData->DisplayPos.y + height;
  const float ortho[16] = {
      2.0F / (R - L), 0.0F, 0.0F, 0.0F,
      0.0F, 2.0F / (T - B), 0.0F, 0.0F,
      0.0F, 0.0F, 0.5F, 0.0F,
      (R + L) / (L - R), (T + B) / (B - T), 0.5F, 1.0F};
  bgfx::setViewTransform(kImGuiViewId, nullptr, ortho);

  const ImVec2 clipPos = drawData->DisplayPos;
  const ImVec2 clipScale = drawData->FramebufferScale;

  for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
    const ImDrawList *drawList = drawData->CmdLists[listIndex];
    const auto vertexCount =
        static_cast<std::uint32_t>(drawList->VtxBuffer.size());
    const auto indexCount =
        static_cast<std::uint32_t>(drawList->IdxBuffer.size());
    if ((vertexCount == 0U) || (indexCount == 0U)) {
      continue;
    }
    if ((bgfx::getAvailTransientVertexBuffer(vertexCount, g_vertexLayout) <
         vertexCount) ||
        (bgfx::getAvailTransientIndexBuffer(indexCount) < indexCount)) {
      break; // out of transient space this frame; drop the remainder
    }

    bgfx::TransientVertexBuffer tvb{};
    bgfx::TransientIndexBuffer tib{};
    bgfx::allocTransientVertexBuffer(&tvb, vertexCount, g_vertexLayout);
    bgfx::allocTransientIndexBuffer(&tib, indexCount);
    std::memcpy(tvb.data, drawList->VtxBuffer.Data,
                vertexCount * sizeof(ImDrawVert));
    std::memcpy(tib.data, drawList->IdxBuffer.Data,
                indexCount * sizeof(ImDrawIdx));

    for (const ImDrawCmd &cmd : drawList->CmdBuffer) {
      if (cmd.UserCallback != nullptr) {
        cmd.UserCallback(drawList, &cmd);
        continue;
      }
      if (cmd.ElemCount == 0U) {
        continue;
      }
      const float clipX = (cmd.ClipRect.x - clipPos.x) * clipScale.x;
      const float clipY = (cmd.ClipRect.y - clipPos.y) * clipScale.y;
      const float clipW = (cmd.ClipRect.z - clipPos.x) * clipScale.x - clipX;
      const float clipH = (cmd.ClipRect.w - clipPos.y) * clipScale.y - clipY;
      if ((clipW <= 0.0F) || (clipH <= 0.0F)) {
        continue;
      }
      bgfx::setScissor(
          static_cast<std::uint16_t>(clipX > 0.0F ? clipX : 0.0F),
          static_cast<std::uint16_t>(clipY > 0.0F ? clipY : 0.0F),
          static_cast<std::uint16_t>(clipW),
          static_cast<std::uint16_t>(clipH));

      bgfx::TextureHandle texture = g_fontTexture;
      if (cmd.GetTexID() != 0) {
        texture.idx = static_cast<std::uint16_t>(
            static_cast<std::uintptr_t>(cmd.GetTexID()));
      }
      bgfx::setTexture(0, g_sampler, texture);
      bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                     BGFX_STATE_MSAA | BGFX_STATE_BLEND_ALPHA);
      bgfx::setVertexBuffer(0, &tvb, cmd.VtxOffset, vertexCount);
      bgfx::setIndexBuffer(&tib, cmd.IdxOffset, cmd.ElemCount);
      bgfx::submit(kImGuiViewId, g_program);
    }
  }
}
