// Declares Game view screenshots in the editor: the Take Screenshot action
// (F9, the Game view's toolbar button, the Edit menu and the `screenshot`
// console command) saves what the Game view shows as a PNG in the project's
// data directory, under Screenshots/, as Unreal's F9 saves into the
// project's Saved/Screenshots. The capture is taken at least one editor
// frame after the request (a Game view tab behind another is brought to the
// front first), and nothing the editor draws over the Game view (a menu, a
// tooltip, the stats overlay) is drawn in that frame, so the picture is the
// game's alone.

#pragma once

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include "editor_project_files.h"

#include "engine/renderer/screenshot.h"

#include <cstddef>
#include <ctime>

namespace engine::editor {

/// Whether a file exists at `path`; the seam next_screenshot_path uses so
/// tests can stage collisions.
using ScreenshotPathExistsFn = ProjectFileExistsFn;

/// next_timestamped_path for a screenshot:
/// `<directory>/Screenshot-YYYYMMDD-HHMMSS.png`, `-2` to `-99` on a
/// collision.
bool next_screenshot_path(const char *directory, const std::tm &time,
                          ScreenshotPathExistsFn exists, void *userData,
                          char *out, std::size_t capacity) noexcept;

/// A rectangle of the editor window in back-buffer pixels, for the part of
/// the Game view image the window shows: `screenPos` and `size` are ImGui
/// points, `viewportPos` the main viewport's position and
/// `framebufferScale` ImGui's DisplayFramebufferScale. The size truncates
/// as the Game view's render target size does, so an unclipped image's
/// region matches it.
renderer::ScreenshotRegion
game_view_screenshot_region(ImVec2 screenPos, ImVec2 size, ImVec2 viewportPos,
                            ImVec2 framebufferScale) noexcept;

/// Asks for a Game view screenshot, taken on a later editor frame (see
/// begin_game_view_screenshot_frame). A second request before then is the
/// same request.
void request_game_view_screenshot() noexcept;

/// Called by the Game view once each frame, before it draws: true on the
/// frame a pending request is taken, when the view must draw nothing over
/// its image. That is the frame after the request once the view has been
/// shown for two frames running (it renders only while shown). A pending
/// request brings a hidden Game view tab to the front, and is dropped with
/// a warning when the view still is not shown ten frames on.
bool begin_game_view_screenshot_frame(bool visible) noexcept;

/// True during the frame a screenshot is being taken.
bool game_view_screenshot_capturing() noexcept;

/// Saves `region` of this frame to the next free name in the project's
/// Screenshots directory, creating it. False, logged, when no project is
/// open, the directory cannot be made, or the renderer refuses the request.
bool take_game_view_screenshot(const renderer::ScreenshotRegion &region) noexcept;

/// Registers the `screenshot` console command. False when the console
/// refuses it (already registered, or full).
bool register_screenshot_command() noexcept;

} // namespace engine::editor
