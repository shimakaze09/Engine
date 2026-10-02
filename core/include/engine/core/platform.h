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
enum class PlatformId : std::uint8_t {
  Windows,
  Linux,
  MacOS,
  Web,
  IOS,
  Android
};

/// What the running platform offers. Callers read this rather than test
/// build macros or infer a capability from a null handle. Every field but
/// hasWindow is fixed per platform (platform_caps_for); hasWindow is the
/// running state.
struct PlatformCaps final {
  PlatformId id = PlatformId::Linux;
  /// A native window a GPU backend can present to. False before the
  /// platform initializes, after it shuts down, and when headless.
  bool hasWindow = false;
  /// The OS drives frames through a callback (the browser's animation
  /// frame, iOS's display link), so the engine must not run its own loop.
  bool ownsMainLoop = false;
  /// Worker threads can be created (on the web, from the page's prewarmed
  /// pool, whose size the build fixes).
  bool hasThreads = true;
  /// Audio stays silent until the user's first gesture (browser
  /// autoplay policy), so the audio device starts or resumes then.
  bool needsAudioUnlock = false;
  /// The main thread may wait: false where blocking it freezes the page
  /// or gets the app killed by a watchdog.
  bool mainThreadMayBlock = true;
  /// Touch is the primary pointer, so UI and input default to it.
  bool touchPrimary = false;
};

/// The fixed capabilities of `id`, with hasWindow false. Pure, so every
/// platform's row is testable on any host.
PlatformCaps platform_caps_for(PlatformId id) noexcept;

/// The running platform's capabilities; valid at any time.
PlatformCaps platform_caps() noexcept;

/// What kind of native window a NativeWindow describes.
enum class NativeWindowKind : std::uint8_t {
  None,
  Win32,
  Cocoa,
  X11,
  Wayland,
  WebCanvas,
  UIKit,
  Android,
};

/// The OS handles behind the platform window, tagged with their kind so a
/// render backend never guesses how to read them (the shape of Rust's
/// raw-window-handle).
struct NativeWindow final {
  NativeWindowKind kind = NativeWindowKind::None;
  /// HWND, NSWindow*, wl_surface*, UIWindow*, ANativeWindow*, or the
  /// canvas's CSS selector string; null for X11 and None.
  void *window = nullptr;
  /// The X11 window id; 0 for every other kind.
  std::uint64_t x11Window = 0U;
  /// The display connection: X11 Display* or wl_display*; null elsewhere.
  void *display = nullptr;
};

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

// ----- Window ----------------------------------------------------------------

/// The window's content scale, for sizing UI to the display: 1.0 at the
/// platform's reference density, 2.0 on a typical HiDPI laptop. 1.0 when
/// there is no window or the platform cannot tell, so a caller can
/// multiply by it unconditionally.
float platform_display_scale() noexcept;

/// The display scale with the window's pixel density divided out: what a
/// UI laid out in window units should scale by. On a Retina Mac the
/// display scale is 2 because the backbuffer has two pixels per window
/// unit, and a renderer drawing at framebuffer size already applies that;
/// the content scale is 1. On Windows at 150% the window units are
/// pixels, so both are 1.5. 1.0 when there is no window.
float platform_content_scale() noexcept;

/// content scale = display scale / pixel density, with 1.0 for either
/// factor when it is not positive. The arithmetic behind
/// platform_content_scale, so every platform's case is testable anywhere.
float content_scale_for(float displayScale, float pixelDensity) noexcept;
/// Sets the window's title. False when there is no window or the platform
/// refuses; the title is copied, so the caller's buffer need not outlive
/// the call.
bool platform_set_window_title(const char *title) noexcept;

/// Presented frames after which the window is shown. The window is created
/// hidden: before its second present it is still being resized to the
/// stored layout and its swapchain holds images nothing has drawn, which a
/// visible window shows as a flash of garbage. The first frame is
/// not enough, because ImGui hides windows created that frame and the
/// swapchain is reset to the final size only after it.
inline constexpr int kPresentsBeforeWindowShown = 2;

/// Counts one presented frame and shows the window once
/// kPresentsBeforeWindowShown have been presented. Idempotent past that; a
/// headless window stays hidden but is counted the same way.
void platform_note_frame_presented() noexcept;

/// True once the window has been shown (or, headless, would have been).
/// Reset when the platform shuts down.
bool platform_window_revealed() noexcept;

// ----- Mouse capture ---------------------------------------------------------

/// Holds the mouse for a drag that turns or moves a camera, as Unreal's
/// and Godot's viewports do: the cursor is hidden and motion keeps
/// arriving as relative deltas (PlatformEvent::deltaX/deltaY) at the
/// screen edge, where an uncaptured cursor stops. True when the mouse is
/// held; false, with the drag left to work uncaptured, when the platform
/// refuses -- a browser that denies pointer lock, a window without focus.
/// The first refusal is logged, the rest are not. Headless there is no
/// cursor, so the capture is only recorded. Main thread only.
bool platform_begin_mouse_capture() noexcept;

/// Lets the mouse go and puts the cursor at (x, y) in window units, where
/// the drag began, so it reappears where the author left it. Harmless
/// when nothing is held. The platform also lets go by itself when the
/// window loses focus and when it shuts down, so a capture never
/// outlives the window's attention. Main thread only.
void platform_end_mouse_capture(float x, float y) noexcept;

/// True while platform_begin_mouse_capture holds the mouse.
bool platform_mouse_captured() noexcept;

/// The main window's size in window units and whether it is maximized.
/// While maximized the size is the one it restores to, so a layout saved
/// maximized still knows its normal size.
struct WindowGeometry final {
  int width = 0;
  int height = 0;
  bool maximized = false;
};

/// Smallest window a restored geometry opens at, in window units.
inline constexpr int kMinRestoredWindowWidth = 640;
inline constexpr int kMinRestoredWindowHeight = 360;

/// The window's current geometry. False when there is no window.
bool platform_window_geometry(WindowGeometry *outGeometry) noexcept;

/// Reopens the window at a stored geometry: the size fitted to the usable
/// area of the display the window is on (fit_window_geometry), the window
/// centred, then maximized if it was. False when there is no window, the
/// geometry has no size, or the platform refuses.
bool platform_apply_window_geometry(const WindowGeometry &geometry) noexcept;

/// A stored geometry fitted to a display's usable area: each side at least
/// the minimum restored size and at most the usable size, so a layout
/// saved on a larger monitor never opens past the edges of a smaller one.
/// A non-positive usable side leaves that side unbounded above.
WindowGeometry fit_window_geometry(const WindowGeometry &stored,
                                   int usableWidth, int usableHeight) noexcept;

// ----- File dialogs ----------------------------------------------------------

/// Open and Save pick a file; Folder picks a directory and takes no
/// filters.
enum class FileDialogKind : std::uint8_t { Open, Save, Folder };

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

/// Shows a native open, save or folder dialog parented to the platform
/// window, starting at `defaultLocation` (may be null), and returns its
/// ticket. kNoFileDialog means no dialog was shown: no window, a bad filter
/// list (a folder dialog takes none), or every slot is held by a dialog
/// that has not closed. The refusal is logged. Main thread only.
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

/// The platform window's native handles for a render backend; kind None
/// when headless, before initialization and after shutdown.
NativeWindow platform_native_window() noexcept;
/// Resident memory of the process in bytes (0 when unsupported).
std::size_t process_memory_bytes() noexcept;

/// Windows: a windowed (GUI-subsystem) build has no console of its own.
/// When the process was started from a terminal, attaches to it and points
/// stdout and stderr there, so a run from a shell still prints, as Godot
/// does; started from Explorer, there is no terminal and nothing changes.
/// A stream the parent redirected to a pipe or a file is left where it
/// goes, so a test or a script capturing the output still gets it. True
/// when a terminal was attached. Everywhere else a no-op returning
/// false: the process already writes to whatever started it.
bool platform_attach_parent_console() noexcept;

/// Shows a modal error box with `title` and `message`: how a windowed
/// application that failed to start says why when it has no console.
/// Callable before or without the platform being initialized; a platform
/// with no way to show one (headless, no display) does nothing.
void platform_show_error_box(const char *title, const char *message) noexcept;

/// Per-user save directory using the engine's default org/app names. On
/// the web it sits under the page's IndexedDB-backed mount, so what is
/// written there survives a reload (see platform_persist_after_write).
bool platform_get_save_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept;
/// Per-user save directory for an explicit org/app pair.
bool platform_get_save_dir(const char *organizationName,
                           const char *applicationName, char *outBuffer,
                           std::size_t bufferCapacity) noexcept;
/// Called once a file at `path` has been committed. On the web, where the
/// save base is an IndexedDB-backed mount the page reads in before main()
/// (core/web/persistent_storage.js), a path under that mount schedules the
/// flush that makes the file outlive the page and returns true; the flush
/// is asynchronous and reports its own failure. Everywhere else, and for
/// any other path, returns false: the file system already persists.
bool platform_persist_after_write(const char *path) noexcept;
/// Directory containing the running executable.
bool platform_get_app_dir(char *outBuffer,
                          std::size_t bufferCapacity) noexcept;
/// OS temp directory.
bool platform_get_temp_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept;

} // namespace engine::core
