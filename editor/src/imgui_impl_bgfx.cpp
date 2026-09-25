// Implements the editor's ImGui renderer for the bgfx backend,
// adapted from bgfx's examples/common/imgui renderer (BSD-2, Branimir
// Karadzic): transient vertex/index buffers per draw list, per-command
// scissors, alpha blending, and the embedded precompiled ocornut-imgui
// shaders. Draws submit into a fixed late view (255) so the UI renders
// after every engine pass regardless of the frame's view allocation.
//
// ImGui owns its textures (ImGuiBackendFlags_RendererHasTextures): it asks
// for its font atlas to be created, updated a rectangle at a time as new
// glyphs are rasterized, and destroyed, so any glyph a font holds renders
// the first time it is drawn -- a CJK name included -- with no glyph
// ranges baked up front. ImTextureID is RenderDevice::native_texture_id's
// value for engine textures and the same encoding for ImGui's own: the bgfx
// handle index plus one, so 0 stays "no texture".

#include "imgui_impl_bgfx.h"

#include "engine/renderer/render_device.h"

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
bgfx::VertexLayout g_vertexLayout{};

ImTextureID encode_texture(bgfx::TextureHandle handle) noexcept {
  return static_cast<ImTextureID>(static_cast<std::uint64_t>(handle.idx) + 1U);
}

/// The bgfx handle an ImTextureID names; invalid for 0.
bgfx::TextureHandle decode_texture(ImTextureID id) noexcept {
  bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
  const auto value = static_cast<std::uint64_t>(id);
  if ((value != 0U) && (value <= 0xFFFFU)) {
    handle.idx = static_cast<std::uint16_t>(value - 1U);
  }
  return handle;
}

/// Uploads one rectangle of an ImGui texture's pixels.
void upload_rect(bgfx::TextureHandle handle, ImTextureData *tex, int x, int y,
                 int w, int h) noexcept {
  if ((w <= 0) || (h <= 0)) {
    return;
  }
  const int pitch = tex->GetPitch();
  // Rows are pitch apart in ImGui's buffer; the copy spans the first row's
  // start to the last row's end and bgfx reads it with the same pitch.
  const auto bytes =
      static_cast<std::uint32_t>((pitch * (h - 1)) + (w * tex->BytesPerPixel));
  bgfx::updateTexture2D(
      handle, 0, 0, static_cast<std::uint16_t>(x),
      static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(w),
      static_cast<std::uint16_t>(h), bgfx::copy(tex->GetPixelsAt(x, y), bytes),
      static_cast<std::uint16_t>(pitch));
}

/// Honours one texture request ImGui queued: create, update or destroy.
void update_texture(ImTextureData *tex) noexcept {
  if (tex->Status == ImTextureStatus_WantCreate) {
    // Only the RGBA32 format is requested (see ImGui_ImplBgfx_Init).
    // Created without memory and then filled like any later update: bgfx
    // makes a texture created with memory immutable and drops every
    // updateTexture2D on it, which is how each glyph ImGui rasterizes
    // after this frame reaches the GPU.
    const bgfx::TextureHandle handle = bgfx::createTexture2D(
        static_cast<std::uint16_t>(tex->Width),
        static_cast<std::uint16_t>(tex->Height), false, 1,
        bgfx::TextureFormat::RGBA8, 0, nullptr);
    if (!bgfx::isValid(handle)) {
      // Left as WantCreate, so the request is retried next frame; the text
      // it holds is invisible until then, which is the most a failed
      // allocation can leave.
      return;
    }
    upload_rect(handle, tex, 0, 0, tex->Width, tex->Height);
    tex->SetTexID(encode_texture(handle));
    tex->SetStatus(ImTextureStatus_OK);
    return;
  }
  if (tex->Status == ImTextureStatus_WantUpdates) {
    const bgfx::TextureHandle handle = decode_texture(tex->GetTexID());
    if (bgfx::isValid(handle)) {
      for (const ImTextureRect &rect : tex->Updates) {
        upload_rect(handle, tex, rect.x, rect.y, rect.w, rect.h);
      }
    }
    tex->SetStatus(ImTextureStatus_OK);
    return;
  }
  // A destroy is honoured only once the texture has gone unused for a
  // frame, since bgfx may still be drawing the previous one with it.
  if ((tex->Status == ImTextureStatus_WantDestroy) && (tex->UnusedFrames > 0)) {
    const bgfx::TextureHandle handle = decode_texture(tex->GetTexID());
    if (bgfx::isValid(handle)) {
      bgfx::destroy(handle);
    }
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
  }
}

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
  io.BackendRendererName = "imgui_impl_bgfx";
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;
  const auto maxSize = static_cast<int>(bgfx::getCaps()->limits.maxTextureSize);
  ImGui::GetPlatformIO().Renderer_TextureMaxWidth = maxSize;
  ImGui::GetPlatformIO().Renderer_TextureMaxHeight = maxSize;
  return true;
}

void ImGui_ImplBgfx_Shutdown() {
  // Once the render device is gone, bgfx::shutdown has reclaimed every
  // handle this backend holds; destroying them again would call into a
  // bgfx that no longer exists.
  const bool deviceLive = engine::renderer::render_device() != nullptr;
  // Textures this context alone uses; a shared one belongs to whichever
  // context outlives this one.
  for (ImTextureData *tex : ImGui::GetPlatformIO().Textures) {
    if (tex->RefCount != 1) {
      continue;
    }
    const bgfx::TextureHandle handle = decode_texture(tex->GetTexID());
    if (deviceLive && bgfx::isValid(handle)) {
      bgfx::destroy(handle);
    }
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
  }
  if (!deviceLive) {
    g_sampler = BGFX_INVALID_HANDLE;
    g_program = BGFX_INVALID_HANDLE;
    return;
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
  // A device that failed after this backend initialized leaves g_program
  // looking valid while bgfx itself is shut down; the device query
  // is the only truth about whether a submit is possible.
  if ((drawData == nullptr) || !bgfx::isValid(g_program) ||
      (engine::renderer::render_device() == nullptr)) {
    return;
  }
  if (drawData->Textures != nullptr) {
    for (ImTextureData *tex : *drawData->Textures) {
      if (tex->Status != ImTextureStatus_OK) {
        update_texture(tex);
      }
    }
  }
  const float width = drawData->DisplaySize.x;
  const float height = drawData->DisplaySize.y;
  // DisplaySize is in logical points; the back buffer (and every scissor
  // below) is in pixels, FramebufferScale apart on HiDPI displays.
  const float fbWidth = width * drawData->FramebufferScale.x;
  const float fbHeight = height * drawData->FramebufferScale.y;
  if ((fbWidth <= 0.0F) || (fbHeight <= 0.0F)) {
    return;
  }

  bgfx::setViewName(kImGuiViewId, "editor-imgui");
  bgfx::setViewMode(kImGuiViewId, bgfx::ViewMode::Sequential);
  bgfx::setViewFrameBuffer(kImGuiViewId, BGFX_INVALID_HANDLE);
  bgfx::setViewRect(kImGuiViewId, 0, 0, static_cast<std::uint16_t>(fbWidth),
                    static_cast<std::uint16_t>(fbHeight));

  // Column-major ortho over logical points: x [L,R] -> [-1,1],
  // y [T,B] -> [1,-1], z [0,1]; the view rect above supplies the scale.
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

      const bgfx::TextureHandle texture = decode_texture(cmd.GetTexID());
      if (!bgfx::isValid(texture)) {
        continue; // a texture whose creation has not succeeded yet
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
