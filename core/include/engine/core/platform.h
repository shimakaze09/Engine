// Declares platform types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Gamepads the platform keeps open and the input layer tracks at once;
/// a controller announced past this many is logged and ignored.
inline constexpr int kMaxGamepads = 4;

// Configuration for window creation.
struct PlatformConfig final {
  int width = 1280;
  int height = 720;
  const char *title = "engine";
  // Hidden window on SDL's dummy video driver, so bootstrap completes on a
  // machine with no display; the render device then stays on the null
  // backend. A visible window is created without an OpenGL context: the
  // render backend owns its device and swapchain and reads the native
  // handles below.
  bool headless = false;
};

/// The platform a build targets.
enum class PlatformId : std::uint8_t { Windows, Linux, MacOS, Web };

/// What the running platform offers. Callers read this rather than test
/// build macros or infer a capability from a null handle.
struct PlatformCaps final {
  PlatformId id = PlatformId::Linux;
  /// A native window a GPU backend can present to. False before the
  /// platform initializes, after it shuts down, and when headless.
  bool hasWindow = false;
};

/// The running platform's capabilities; valid at any time.
PlatformCaps platform_caps() noexcept;

/// Nanoseconds on the monotonic clock PlatformEvent::timestampNs is read
/// from, so a caller can place an event in time relative to now.
std::uint64_t platform_ticks_ns() noexcept;

/// Initializes the owning system for platform.
bool initialize_platform() noexcept;
/// Initializes the owning system for platform.
bool initialize_platform(const PlatformConfig &config) noexcept;
/// Shuts down the owning system for platform.
void shutdown_platform() noexcept;
/// Returns whether is platform running.
bool is_platform_running() noexcept;
/// Requests the platform loop to exit after the current frame.
void request_platform_quit() noexcept;
/// Drawable size in pixels (may differ from window size on HiDPI).
void render_drawable_size(int *outWidth, int *outHeight) noexcept;
/// Copies the environment variable's value into `out`; false, with `out`
/// emptied, when the variable is unset or empty, or the value does not
/// fit `capacity` whole. The caller owns the bytes, so two lookups on
/// one thread never alias (the Windows path reads through the Win32
/// API, avoiding the CRT getenv deprecation).
bool non_empty_env(const char *name, char *out, std::size_t capacity) noexcept;

/// Fills `out` with `size` bytes from the OS entropy source; false with
/// `out` untouched when the platform refuses. Cold path only — it may
/// open a device file — and intended for generating a persistent
/// identity in an explicit transaction, never per frame and never on a
/// deterministic read or cook path.
bool platform_random_bytes(void *out, std::size_t size) noexcept;

// ----- Gamepad devices -------------------------------------------------------
// The platform owns the OS-level gamepad subsystem and the open device
// handles: SDL announces a controller and delivers its button and axis
// events only for devices that were opened, so the input layer asks the
// platform to open a device on its hotplug arrival and to close it on
// removal. Device identity is the instance id the arrival event carries.

/// True when the gamepad subsystem initialized with the platform; false
/// when it is unavailable (then no device is ever opened and the input
/// layer sees only synthetic events).
bool platform_gamepads_available() noexcept;
/// Opens the device behind an instance id so its events are delivered;
/// a failure (subsystem unavailable, table full, device refused) is logged
/// and leaves the device closed. Idempotent for an already open id.
void platform_open_gamepad(std::uint32_t instanceId) noexcept;
/// Closes the device behind an instance id; a no-op for an unknown id.
void platform_close_gamepad(std::uint32_t instanceId) noexcept;
/// Underlying SDL_Window* (opaque). Its one sanctioned consumer is the
/// editor's ImGui SDL3 backend, which is written against SDL and needs the
/// window itself; everything else asks the platform for what it wants
/// from the window through the functions below.
void *get_sdl_window() noexcept;

// ----- Window ----------------------------------------------------------------

/// The window's content scale, for sizing UI to the display: 1.0 at the
/// platform's reference density, 2.0 on a typical HiDPI laptop. 1.0 when
/// there is no window or the platform cannot tell, so a caller can
/// multiply by it unconditionally.
float platform_display_scale() noexcept;
/// Sets the window's title. False when there is no window or the platform
/// refuses; the title is copied, so the caller's buffer need not outlive
/// the call.
bool platform_set_window_title(const char *title) noexcept;

// ----- File dialogs ----------------------------------------------------------

enum class FileDialogKind : std::uint8_t { Open, Save };

/// One entry in a dialog's file-type list: a display name and a
/// semicolon-separated list of extensions without dots ("scene",
/// "png;jpg"). Read after the call returns, until the dialog closes, so
/// both strings must outlive it -- in practice, string literals.
struct FileDialogFilter final {
  const char *name = nullptr;
  const char *pattern = nullptr;
};

/// Filters one dialog can carry.
inline constexpr int kMaxFileDialogFilters = 4;

/// Names one dialog request. Zero is never a live ticket.
using FileDialogTicket = std::uint32_t;
inline constexpr FileDialogTicket kNoFileDialog = 0U;

/// Dialogs that can be outstanding at once. A dialog stays outstanding
/// until the OS closes it, even when its requester has abandoned it, so
/// the table has room for a few such stragglers.
inline constexpr int kMaxPendingFileDialogs = 4;
/// Longest path, terminator included, a dialog result carries.
inline constexpr std::size_t kMaxFileDialogPathLength = 1024U;

enum class FileDialogOutcome : std::uint8_t {
  Chosen,
  Cancelled,
  /// The native dialog failed; the platform logged why.
  Failed,
  /// The user chose a path longer than kMaxFileDialogPathLength. It is
  /// refused rather than cut, because a cut path names a different file.
  PathTooLong,
};

/// One dialog's result, owned by the caller once taken.
struct FileDialogResult final {
  FileDialogTicket ticket = kNoFileDialog;
  FileDialogOutcome outcome = FileDialogOutcome::Cancelled;
  /// The chosen path when outcome is Chosen, otherwise empty.
  char path[kMaxFileDialogPathLength] = {};
};

enum class FileDialogPoll : std::uint8_t {
  /// The dialog is still open.
  Pending,
  /// The result was copied out and the ticket is spent.
  Ready,
  /// Not a live ticket: never issued, already taken, or abandoned.
  Unknown,
};

/// Shows a native open or save dialog parented to the platform window,
/// starting at `defaultLocation` (may be null), and returns its ticket.
/// kNoFileDialog means no dialog was shown: no window, a bad filter list,
/// or every slot is held by a dialog that has not closed. The refusal is
/// logged. Main thread only.
///
/// The OS answers on a thread of its choosing (a portal worker on Linux).
/// The platform keeps the answer until the requester takes it with
/// platform_take_file_dialog_result, so the requester only ever sees a
/// result on the main thread.
FileDialogTicket
platform_request_file_dialog(FileDialogKind kind,
                             const FileDialogFilter *filters, int filterCount,
                             const char *defaultLocation) noexcept;

/// Copies out and spends the ticket's result once the dialog has closed.
/// Main thread only.
FileDialogPoll platform_take_file_dialog_result(FileDialogTicket ticket,
                                                FileDialogResult *out) noexcept;

/// Gives up a ticket: its result, whenever it arrives, is dropped, and
/// its slot is freed once the OS has closed the dialog. No platform
/// promises a synchronous cancel, so the dialog itself stays open.
/// Harmless on a ticket that is not live. Main thread only.
void platform_abandon_file_dialog(FileDialogTicket ticket) noexcept;

/// Headless automation and tests: while enabled, requests take a slot and
/// a ticket but show nothing and need no window. Each is then answered
/// with platform_answer_scripted_file_dialog. Main thread only.
void platform_set_scripted_file_dialogs(bool enabled) noexcept;

/// Answers a scripted request as the OS would: a path is the user's
/// choice, null is a cancel. It goes through the same delivery as a
/// native answer, so it may be called from any thread. False when the
/// ticket is not an unanswered scripted request.
bool platform_answer_scripted_file_dialog(FileDialogTicket ticket,
                                          const char *path) noexcept;

/// Native window handle for external render backends: X11 window
/// id / Wayland wl_surface / Win32 HWND / Cocoa NSWindow, null when
/// headless or before initialization.
void *platform_native_window_handle() noexcept;
/// Native display handle for external render backends (X11 Display /
/// Wayland wl_display); null on platforms without a display connection.
void *platform_native_display_handle() noexcept;
/// True when the platform window runs on the Wayland video driver (an
/// external backend must use Wayland platform-data semantics).
bool platform_window_is_wayland() noexcept;
/// Resident memory of the process in bytes (0 when unsupported).
std::size_t process_memory_bytes() noexcept;

/// Per-user save directory using the engine's default org/app names.
bool platform_get_save_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept;
/// Per-user save directory for an explicit org/app pair.
bool platform_get_save_dir(const char *organizationName,
                           const char *applicationName, char *outBuffer,
                           std::size_t bufferCapacity) noexcept;
/// Directory containing the running executable.
bool platform_get_app_dir(char *outBuffer,
                          std::size_t bufferCapacity) noexcept;
/// OS temp directory.
bool platform_get_temp_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept;

} // namespace engine::core
