// Engine-facing render-device contract: backend-neutral resources
// (buffers, textures, programs, geometry, render targets, queries),
// descriptors, shader parameters, render state, and capabilities. The
// vocabulary here describes engine intent; GL/bgfx mechanics (VAOs,
// uniform locations, texture units, FBO ids) stay inside backend
// implementations and must not appear in this header (#165).

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

// --- Resource handles -----------------------------------------------------
//
// Every device resource is addressed by a 32-bit generational handle
// (slot + generation, value 0 invalid). The backend validates slot and
// generation on use: operations on invalid, stale, or destroyed handles
// are dropped and counted (see DeviceDebugStats) instead of aliasing a
// recycled backend object. Destroy calls are idempotent. The creator of
// a resource owns it and must destroy it; destruction is immediate.
// shutdown_render_device invalidates every outstanding handle.

/// Device buffer (vertex/index/uniform storage); 0 = invalid.
struct DeviceBufferHandle final {
  std::uint32_t value = 0U;

  friend constexpr bool operator==(const DeviceBufferHandle &,
                                   const DeviceBufferHandle &) = default;
};

/// Device texture (2D or cubemap); 0 = invalid.
struct DeviceTextureHandle final {
  std::uint32_t value = 0U;

  friend constexpr bool operator==(const DeviceTextureHandle &,
                                   const DeviceTextureHandle &) = default;
};

/// Linked shader program owned by the device; 0 = invalid.
struct DeviceProgramHandle final {
  std::uint32_t value = 0U;

  friend constexpr bool operator==(const DeviceProgramHandle &,
                                   const DeviceProgramHandle &) = default;
};

/// Drawable geometry: vertex stream layout plus optional index stream.
struct DeviceGeometryHandle final {
  std::uint32_t value = 0U;

  friend constexpr bool operator==(const DeviceGeometryHandle &,
                                   const DeviceGeometryHandle &) = default;
};

/// Render target (color/depth attachment set); 0 = the back buffer.
struct RenderTargetHandle final {
  std::uint32_t value = 0U;

  friend constexpr bool operator==(const RenderTargetHandle &,
                                   const RenderTargetHandle &) = default;
};

/// GPU timestamp query; 0 = invalid.
struct DeviceQueryHandle final {
  std::uint32_t value = 0U;

  friend constexpr bool operator==(const DeviceQueryHandle &,
                                   const DeviceQueryHandle &) = default;
};

inline constexpr DeviceBufferHandle kInvalidDeviceBuffer{};
inline constexpr DeviceTextureHandle kInvalidDeviceTexture{};
inline constexpr DeviceProgramHandle kInvalidDeviceProgram{};
inline constexpr DeviceGeometryHandle kInvalidDeviceGeometry{};
inline constexpr DeviceQueryHandle kInvalidDeviceQuery{};
/// Binding the back-buffer render target.
inline constexpr RenderTargetHandle kBackBufferTarget{};

// --- Shader parameters ----------------------------------------------------

/// Opaque token for one shader/material input of a program, resolved by
/// name through shader_param(). Invalid tokens (parameter absent from the
/// current link) are explicit: setting through an invalid token is a
/// defined no-op, never backend sentinel behavior. Tokens are only valid
/// for the program they were resolved from and become stale when that
/// program is relinked (see shader_reload_epoch in shader_system.h).
struct ShaderParam final {
  std::int32_t value = -1;

  /// True when the parameter exists in the resolved program link.
  constexpr bool valid() const noexcept { return value >= 0; }

  friend constexpr bool operator==(const ShaderParam &,
                                   const ShaderParam &) = default;
};

inline constexpr ShaderParam kInvalidShaderParam{};

// --- Resource descriptors -------------------------------------------------

/// What a buffer feeds; fixed at creation so updates and draws never
/// name backend bind targets.
enum class BufferUsage : std::uint8_t {
  Vertex = 0,
  Index = 1,
  Uniform = 2,
};

/// Update frequency intent for a buffer's storage.
enum class BufferAccess : std::uint8_t {
  Static = 0, // uploaded once (or rarely) after creation
  Stream = 1, // re-uploaded per frame/batch
};

/// Creation parameters for a device buffer. data may be null to allocate
/// uninitialized storage of sizeBytes (sizeBytes 0 with null data defers
/// allocation to the first update).
struct BufferDesc final {
  BufferUsage usage = BufferUsage::Vertex;
  BufferAccess access = BufferAccess::Static;
  std::ptrdiff_t sizeBytes = 0;
  const void *data = nullptr;
};

/// Texture dimensionality.
enum class TextureKind : std::uint8_t {
  Tex2D = 0,
  Cube = 1,
};

/// Backend-neutral texel formats used by the engine.
enum class TextureFormat : std::uint8_t {
  R8 = 0,
  RG8 = 1,
  RGB8 = 2,
  RGBA8 = 3,
  R16F = 4,
  RG16F = 5,
  RGB16F = 6,
  RGBA16F = 7,
  R32F = 8,
  Depth24 = 9,
};

/// Texture sampling filter intent.
enum class TextureFilter : std::uint8_t {
  Nearest = 0,
  Linear = 1,
  LinearMipmap = 2, // requires mips (generated or allocated)
};

/// Texture coordinate wrap intent.
enum class TextureWrap : std::uint8_t {
  Repeat = 0,
  ClampEdge = 1,
};

/// Source pixel component encoding for uploads.
enum class TexelData : std::uint8_t {
  U8 = 0,
  F32 = 1,
};

/// Creation parameters for a device texture. For Tex2D provide pixels
/// (or null for an empty target); for Cube provide facePixels (each may
/// be null). mipLevels 0 generates a full chain from the level-0 data;
/// 1 means no mips; >1 allocates that many empty levels (render-to-mip
/// targets). pixelData must match the format's component encoding.
struct TextureDesc final {
  TextureKind kind = TextureKind::Tex2D;
  TextureFormat format = TextureFormat::RGBA8;
  std::int32_t width = 0;  // Cube: face size
  std::int32_t height = 0; // Cube: ignored (faces are square)
  std::int32_t mipLevels = 1;
  TextureFilter filter = TextureFilter::Linear;
  TextureWrap wrap = TextureWrap::ClampEdge;
  TexelData pixelData = TexelData::U8;
  const void *pixels = nullptr;
  const void *const *facePixels = nullptr; // 6 entries when kind == Cube
};

/// Engine vertex input semantics; the backend maps each to its own
/// attribute binding model.
enum class VertexSemantic : std::uint8_t {
  Position = 0,
  Normal = 1,
  TexCoord0 = 2,
  Joints = 3,
  Weights = 4,
  Color = 5,
  InstanceModel0 = 6, // four columns of the per-instance model matrix
  InstanceModel1 = 7,
  InstanceModel2 = 8,
  InstanceModel3 = 9,
  InstanceParams = 10,
};

/// One float vertex attribute inside an interleaved stream.
struct VertexAttribute final {
  VertexSemantic semantic = VertexSemantic::Position;
  std::int32_t componentCount = 0;
  std::int32_t offsetBytes = 0;
};

inline constexpr std::size_t kMaxVertexAttributes = 8U;

/// Interleaved float vertex stream layout.
struct VertexLayout final {
  std::int32_t strideBytes = 0;
  std::size_t attributeCount = 0U;
  VertexAttribute attributes[kMaxVertexAttributes] = {};
};

/// Creation parameters for drawable geometry. vertexBuffer may be
/// invalid for attribute-less draws (fullscreen triangles); indexBuffer
/// is optional (32-bit indices when present).
struct GeometryDesc final {
  DeviceBufferHandle vertexBuffer{};
  VertexLayout layout{};
  DeviceBufferHandle indexBuffer{};
};

/// Cubemap face selector for render-target attachments.
enum class CubeFace : std::int8_t {
  None = -1, // 2D attachment
  PositiveX = 0,
  NegativeX = 1,
  PositiveY = 2,
  NegativeY = 3,
  PositiveZ = 4,
  NegativeZ = 5,
};

/// One render-target attachment: a texture level (and cube face when the
/// texture is a cubemap).
struct RenderTargetAttachment final {
  DeviceTextureHandle texture{};
  CubeFace face = CubeFace::None;
  std::int32_t mipLevel = 0;
};

inline constexpr std::size_t kMaxColorAttachments = 4U;

/// Creation parameters for a render target. Attachments are fixed for
/// the target's lifetime; completeness is validated at creation and
/// creation fails (invalid handle) when the combination is unsupported.
/// Depth-only targets are valid (shadow maps).
struct RenderTargetDesc final {
  std::size_t colorCount = 0U;
  RenderTargetAttachment colors[kMaxColorAttachments] = {};
  RenderTargetAttachment depth{};
};

// --- Pipeline state -------------------------------------------------------

/// Depth-test intent for subsequent draws.
enum class DepthTest : std::uint8_t {
  Disabled = 0,
  Less = 1,
  LessEqual = 2,
};

/// Blend intent for subsequent draws.
enum class BlendMode : std::uint8_t {
  Disabled = 0,
  Alpha = 1, // src-alpha / one-minus-src-alpha
};

/// Face-culling intent for subsequent draws.
enum class CullMode : std::uint8_t {
  None = 0,
  Back = 1,
};

/// Whole render state applied at pass boundaries; every field is applied
/// on each call so passes cannot inherit stale state from one another.
struct RenderState final {
  DepthTest depthTest = DepthTest::Disabled;
  bool depthWrite = true;
  BlendMode blend = BlendMode::Disabled;
  CullMode cull = CullMode::None;
};

/// Clear-operation selector; values combine bitwise.
enum class ClearFlags : std::uint8_t {
  Color = 1U << 0U,
  Depth = 1U << 1U,
  ColorDepth = (1U << 0U) | (1U << 1U),
};

/// Geometry primitive interpretation for non-indexed draws.
enum class PrimitiveTopology : std::uint8_t {
  Triangles = 0,
  Lines = 1,
};

// --- Capabilities ---------------------------------------------------------

/// Optional-feature contract. A false flag means the feature's operations
/// report failure or are dropped; callers gate through these flags rather
/// than probing backend identity.
struct DeviceCaps final {
  bool instancing = false;      // draw_indexed_instanced + instance streams
  bool uniformBlocks = false;   // uniform buffers + program block binding
  bool timestampQueries = false;
  bool cookedPrograms = false;  // create_program_binary from cooked shaders
};

/// Counters for dropped invalid-handle/invalid-argument operations; a
/// nonzero droppedOperations after a frame indicates a contract violation
/// upstream (stale handle, bad descriptor) that the backend refused.
struct DeviceDebugStats final {
  std::uint64_t droppedOperations = 0U;
};

/// Function table over the active backend; null entries mean the entry is
/// unavailable (partial tables appear in tests; production backends fill
/// every entry and express optional features through caps).
struct RenderDevice final {
  DeviceCaps caps{};

  // Buffers. Creation returns an invalid handle on failure (logged).
  DeviceBufferHandle (*create_buffer)(const BufferDesc &desc) noexcept =
      nullptr;
  // Re-specifies the buffer's full contents (storage may be reallocated).
  void (*update_buffer)(DeviceBufferHandle buffer, const void *data,
                        std::ptrdiff_t sizeBytes) noexcept = nullptr;
  // Overwrites a prefix of the existing storage without reallocation.
  void (*update_buffer_range)(DeviceBufferHandle buffer, const void *data,
                              std::ptrdiff_t sizeBytes) noexcept = nullptr;
  void (*destroy_buffer)(DeviceBufferHandle buffer) noexcept = nullptr;
  // Attaches a uniform buffer to a program-visible binding slot.
  void (*bind_uniform_buffer_slot)(std::uint32_t slot,
                                   DeviceBufferHandle buffer) noexcept =
      nullptr;

  // Textures.
  DeviceTextureHandle (*create_texture)(const TextureDesc &desc) noexcept =
      nullptr;
  // Level-0 upload anchored at the top-left texel; width/height must not
  // exceed the creation shape (equal for a full re-upload, smaller for a
  // partial row/column update such as the tile-light table).
  void (*update_texture)(DeviceTextureHandle texture, const void *pixels,
                         std::int32_t width,
                         std::int32_t height) noexcept = nullptr;
  void (*destroy_texture)(DeviceTextureHandle texture) noexcept = nullptr;
  // Makes the texture readable by shader samplers pointed at `slot`
  // (see set_param_i32 for sampler-to-slot assignment). An invalid
  // handle unbinds the slot.
  void (*bind_texture_slot)(std::uint32_t slot,
                            DeviceTextureHandle texture) noexcept = nullptr;

  // Programs — compile + link below the boundary (errors logged; invalid
  // handle on failure).
  DeviceProgramHandle (*create_program)(const char *vertexSource,
                                        const char *fragmentSource) noexcept =
      nullptr;
  // Links a program from cooked shader binaries (#138 Phase C): opaque
  // backend-cooked bytes in, program handle out. Requires
  // caps.cookedPrograms; source-compiling backends leave it null.
  DeviceProgramHandle (*create_program_binary)(
      const void *vertexData, std::ptrdiff_t vertexSize,
      const void *fragmentData, std::ptrdiff_t fragmentSize) noexcept =
      nullptr;
  // Stable engine profile tag ("glsl", "essl", "spirv", "metal") naming
  // the cooked shader flavor this backend consumes; pairs with
  // caps.cookedPrograms.
  const char *(*cooked_program_profile)() noexcept = nullptr;
  void (*destroy_program)(DeviceProgramHandle program) noexcept = nullptr;
  // Makes the program current for subsequent set_param_*/draw calls; an
  // invalid handle unbinds.
  void (*bind_program)(DeviceProgramHandle program) noexcept = nullptr;

  // Shader parameters. Resolution is per program link; setters act on the
  // currently bound program and are defined no-ops for invalid params.
  ShaderParam (*shader_param)(DeviceProgramHandle program,
                              const char *name) noexcept = nullptr;
  void (*set_param_mat4)(ShaderParam param, const float *value) noexcept =
      nullptr;
  void (*set_param_mat3)(ShaderParam param, const float *value) noexcept =
      nullptr;
  void (*set_param_f32)(ShaderParam param, float value) noexcept = nullptr;
  void (*set_param_i32)(ShaderParam param, std::int32_t value) noexcept =
      nullptr;
  void (*set_param_vec2)(ShaderParam param, const float *value) noexcept =
      nullptr;
  void (*set_param_vec3)(ShaderParam param, const float *value) noexcept =
      nullptr;
  void (*set_param_vec4)(ShaderParam param, const float *value) noexcept =
      nullptr;
  // Binds the program's named uniform block to a buffer binding slot;
  // false when the link does not expose the block (callers treat a
  // required block like a missing required parameter).
  bool (*bind_program_uniform_block)(DeviceProgramHandle program,
                                     const char *blockName,
                                     std::uint32_t slot) noexcept = nullptr;

  // Geometry.
  DeviceGeometryHandle (*create_geometry)(const GeometryDesc &desc) noexcept =
      nullptr;
  void (*destroy_geometry)(DeviceGeometryHandle geometry) noexcept = nullptr;
  // Attaches/replaces the geometry's per-instance vertex stream (layout
  // attributes advance once per instance). Requires caps.instancing.
  bool (*set_geometry_instance_stream)(DeviceGeometryHandle geometry,
                                       DeviceBufferHandle buffer,
                                       const VertexLayout &layout) noexcept =
      nullptr;

  // Draws (geometry selected per draw; invalid geometry drops the draw).
  void (*draw)(DeviceGeometryHandle geometry, PrimitiveTopology topology,
               std::int32_t firstVertex,
               std::int32_t vertexCount) noexcept = nullptr;
  void (*draw_indexed)(DeviceGeometryHandle geometry,
                       std::int32_t indexCount) noexcept = nullptr;
  void (*draw_indexed_instanced)(DeviceGeometryHandle geometry,
                                 std::int32_t indexCount,
                                 std::int32_t instanceCount) noexcept = nullptr;

  // Render targets.
  RenderTargetHandle (*create_render_target)(
      const RenderTargetDesc &desc) noexcept = nullptr;
  void (*destroy_render_target)(RenderTargetHandle target) noexcept = nullptr;
  // kBackBufferTarget selects the window back buffer.
  void (*bind_render_target)(RenderTargetHandle target) noexcept = nullptr;
  // Copies the depth attachment contents between equal-sized targets.
  void (*copy_depth)(RenderTargetHandle source, RenderTargetHandle destination,
                     std::int32_t width,
                     std::int32_t height) noexcept = nullptr;

  // State, viewport, clear.
  void (*apply_render_state)(const RenderState &state) noexcept = nullptr;
  void (*set_viewport)(std::int32_t x, std::int32_t y, std::int32_t w,
                       std::int32_t h) noexcept = nullptr;
  void (*clear)(ClearFlags flags, float r, float g, float b,
                float a) noexcept = nullptr;

  // GPU timestamp queries (caps.timestampQueries).
  DeviceQueryHandle (*create_timestamp_query)() noexcept = nullptr;
  void (*destroy_timestamp_query)(DeviceQueryHandle query) noexcept = nullptr;
  void (*write_timestamp)(DeviceQueryHandle query) noexcept = nullptr;
  bool (*timestamp_ready)(DeviceQueryHandle query) noexcept = nullptr;
  std::uint64_t (*timestamp_value)(DeviceQueryHandle query) noexcept = nullptr;

  // UI-backend escape hatch: the native texture id consumed by the UI
  // renderer running on the same backend (ImGui image binding). Not part
  // of the engine rendering contract; nothing else may interpret it.
  std::uint64_t (*native_texture_id)(DeviceTextureHandle texture) noexcept =
      nullptr;

  // Dropped-operation diagnostics (stale handles, invalid descriptors).
  DeviceDebugStats (*debug_stats)() noexcept = nullptr;
};

/// Initializes the owning system for render device.
bool initialize_render_device() noexcept;
/// Shuts down the owning system for render device.
void shutdown_render_device() noexcept;
/// Active device table, or nullptr before initialization.
const RenderDevice *render_device() noexcept;

} // namespace engine::renderer
