// Implements shader system behavior for the Engine renderer system.

#include "engine/renderer/shader_system.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/core/vfs.h"
#include "engine/renderer/material.h"
#include "engine/renderer/render_device.h"
#include "shader_system_internal.h"

namespace engine::renderer {

std::size_t shader_version_prefix_length(const char *source) noexcept {
  std::size_t lineStart = 0U;
  while (source[lineStart] != '\0') {
    std::size_t i = lineStart;
    while ((source[i] == ' ') || (source[i] == '\t')) {
      ++i;
    }
    if (std::strncmp(source + i, "#version", 8U) == 0) {
      while ((source[i] != '\0') && (source[i] != '\n')) {
        ++i;
      }
      if (source[i] == '\n') {
        ++i;
      }
      return i;
    }
    while ((source[lineStart] != '\0') && (source[lineStart] != '\n')) {
      ++lineStart;
    }
    if (source[lineStart] == '\n') {
      ++lineStart;
    }
  }
  return 0U;
}

namespace {

constexpr std::size_t kMaxShaderPrograms = 64U;
constexpr std::size_t kMaxPathLength = 128U;
constexpr std::size_t kMaxShaderVariants = kMaxShaderPrograms;
constexpr std::size_t kMaxShaderDefines = 16U;
constexpr std::size_t kMaxDefineNameLength = 48U;
constexpr std::size_t kMaxDefineValueLength = 64U;
constexpr const char *kEnabledDefineValue = "1";

/// Stores copied shader define strings for reloadable variants.
struct ShaderDefineCopy final {
  char name[kMaxDefineNameLength] = {};
  char value[kMaxDefineValueLength] = {};
};

/// Copies a shader VFS path into a registry entry.
void safe_copy_path(char *dst, const char *src) noexcept {
  core::copy_string(dst, kMaxPathLength, src);
}

/// Stores one cached shader variant key and its compiled program handle.
struct ShaderVariantEntry final {
  bool active = false;
  ShaderVariantKey key{};
  ShaderProgramHandle handle{};
};

struct ShaderEntry final {
  bool active = false;
  std::uint32_t generation = 1U;
  char vertPath[kMaxPathLength] = {};
  char fragPath[kMaxPathLength] = {};
  ShaderDefineCopy defines[kMaxShaderDefines] = {};
  std::size_t defineCount = 0U;
  DeviceProgramHandle deviceProgram{};
  std::int64_t vertMtime = 0;
  std::int64_t fragMtime = 0;
};

ShaderEntry g_entries[kMaxShaderPrograms] = {};
ShaderVariantEntry g_variants[kMaxShaderVariants] = {};
bool g_initialized = false;

/// Never reset, so external caches always observe a change after any
/// relink — including one that straddles a shader-system restart.
std::uint64_t g_reloadEpoch = 0U;

std::uint32_t next_generation(std::uint32_t generation) noexcept {
  ++generation;
  return generation == 0U ? 1U : generation;
}

void reset_shader_entry(std::size_t index) noexcept {
  const std::uint32_t generation = next_generation(g_entries[index].generation);
  g_entries[index] = ShaderEntry{};
  g_entries[index].generation = generation;
}

bool is_current_handle(ShaderProgramHandle handle) noexcept {
  if ((handle.id == 0U) || (handle.id > kMaxShaderPrograms) ||
      !g_initialized) {
    return false;
  }

  const ShaderEntry &entry = g_entries[handle.id - 1U];
  return entry.active && (entry.generation == handle.generation);
}

/// Adds one enabled material shader define to the selection.
void append_material_shader_define(MaterialShaderVariantSelection &selection,
                                   const char *name) noexcept {
  if (selection.defineCount >= kMaxMaterialShaderDefines) {
    return;
  }

  selection.defines[selection.defineCount] = ShaderDefine{name,
                                                          kEnabledDefineValue};
  ++selection.defineCount;
}

/// Returns whether any emissive channel contributes light.
bool has_emissive_value(const math::Vec3 &emissive) noexcept {
  return (emissive.x != 0.0F) || (emissive.y != 0.0F) ||
         (emissive.z != 0.0F);
}

/// Returns the effective value used for valueless shader defines.
const char *normalized_define_value(const ShaderDefine &define) noexcept {
  if ((define.value == nullptr) || (define.value[0] == '\0')) {
    return "1";
  }
  return define.value;
}

/// Checks whether a string fits in a fixed-size copied field.
bool string_fits(const char *text, std::size_t fieldSize) noexcept {
  if ((text == nullptr) || (fieldSize == 0U)) {
    return false;
  }

  std::size_t i = 0U;
  while ((i < fieldSize) && (text[i] != '\0')) {
    ++i;
  }
  return i < fieldSize;
}

/// Compares two shader defines by their normalized name and value.
int compare_shader_define(const ShaderDefine &a,
                          const ShaderDefine &b) noexcept {
  const int nameOrder = std::strcmp(a.name, b.name);
  if (nameOrder != 0) {
    return nameOrder;
  }
  return std::strcmp(normalized_define_value(a),
                     normalized_define_value(b));
}

/// Validates one shader define before it is copied or hashed.
bool validate_shader_define(const ShaderDefine &define) noexcept {
  if ((define.name == nullptr) || (define.name[0] == '\0')) {
    return false;
  }
  if (!string_fits(define.name, kMaxDefineNameLength)) {
    return false;
  }
  return string_fits(normalized_define_value(define), kMaxDefineValueLength);
}

/// Validates a variant descriptor before cache lookup or compilation.
bool validate_shader_variant_desc(const ShaderVariantDesc &desc) noexcept {
  if ((desc.vertPath == nullptr) || (desc.fragPath == nullptr)) {
    return false;
  }
  if (!string_fits(desc.vertPath, kMaxPathLength) ||
      !string_fits(desc.fragPath, kMaxPathLength)) {
    return false;
  }
  if (desc.defineCount > kMaxShaderDefines) {
    return false;
  }
  if ((desc.defineCount > 0U) && (desc.defines == nullptr)) {
    return false;
  }

  for (std::size_t i = 0U; i < desc.defineCount; ++i) {
    if (!validate_shader_define(desc.defines[i])) {
      return false;
    }
    for (std::size_t j = i + 1U; j < desc.defineCount; ++j) {
      if ((desc.defines[j].name != nullptr) &&
          (std::strcmp(desc.defines[i].name, desc.defines[j].name) == 0)) {
        return false;
      }
    }
  }

  return true;
}

/// Adds a null-terminated string plus a separator to an FNV-1a hash.
void hash_string(std::uint64_t &hash, const char *text) noexcept {
  for (std::size_t i = 0U; text[i] != '\0'; ++i) {
    hash = core::fnv1a_64_append(hash, static_cast<std::uint8_t>(text[i]));
  }
  hash = core::fnv1a_64_append(hash, 0U);
}

/// Sorts shader define pointers so a define set has an order-independent key.
void sort_shader_defines(
    std::array<const ShaderDefine *, kMaxShaderDefines> &sorted,
    std::size_t count) noexcept {
  for (std::size_t i = 1U; i < count; ++i) {
    const ShaderDefine *current = sorted[i];
    std::size_t j = i;
    while ((j > 0U) && (compare_shader_define(*current, *sorted[j - 1U]) < 0)) {
      sorted[j] = sorted[j - 1U];
      --j;
    }
    sorted[j] = current;
  }
}

/// Copies descriptor defines into a reloadable shader registry entry.
void copy_shader_defines(ShaderEntry &entry, const ShaderDefine *defines,
                         std::size_t defineCount) noexcept {
  entry.defineCount = defineCount;
  for (std::size_t i = 0U; i < defineCount; ++i) {
    core::copy_string(entry.defines[i].name, kMaxDefineNameLength,
                     defines[i].name);
    core::copy_string(entry.defines[i].value, kMaxDefineValueLength,
                     normalized_define_value(defines[i]));
  }
}

/// Counts the bytes needed for all variant define lines.
std::size_t shader_define_preamble_size(const ShaderDefineCopy *defines,
                                        std::size_t defineCount) noexcept {
  std::size_t total = 0U;
  for (std::size_t i = 0U; i < defineCount; ++i) {
    total += 8U + std::strlen(defines[i].name) + 1U +
             std::strlen(defines[i].value) + 1U;
  }
  return total;
}

/// Appends a byte range to an output cursor.
void append_chars(char *&out, const char *text, std::size_t count) noexcept {
  if (count == 0U) {
    return;
  }
  std::memcpy(out, text, count);
  out += count;
}

/// Builds shader source with variant defines inserted after #version.
std::unique_ptr<char[]> build_source_with_defines(
    const char *source, const ShaderDefineCopy *defines,
    std::size_t defineCount) noexcept {
  const std::size_t sourceLength = std::strlen(source);
  const std::size_t versionLength = shader_version_prefix_length(source);
  const bool versionNeedsNewline =
      (versionLength > 0U) && (source[versionLength - 1U] != '\n');
  const std::size_t preambleLength =
      shader_define_preamble_size(defines, defineCount);
  const std::size_t outputLength =
      sourceLength + preambleLength + (versionNeedsNewline ? 1U : 0U);

  std::unique_ptr<char[]> output(new (std::nothrow) char[outputLength + 1U]);
  if (!output) {
    return nullptr;
  }

  char *cursor = output.get();
  append_chars(cursor, source, versionLength);
  if (versionNeedsNewline) {
    *cursor = '\n';
    ++cursor;
  }

  for (std::size_t i = 0U; i < defineCount; ++i) {
    append_chars(cursor, "#define ", 8U);
    append_chars(cursor, defines[i].name, std::strlen(defines[i].name));
    append_chars(cursor, " ", 1U);
    append_chars(cursor, defines[i].value, std::strlen(defines[i].value));
    append_chars(cursor, "\n", 1U);
  }

  append_chars(cursor, source + versionLength, sourceLength - versionLength);
  *cursor = '\0';
  return output;
}

/// Removes cached variant keys that reference a shader program handle.
void remove_variants_for_handle(ShaderProgramHandle handle) noexcept {
  for (auto &variant : g_variants) {
    if (variant.active && (variant.handle == handle)) {
      variant = ShaderVariantEntry{};
    }
  }
}

DeviceProgramHandle compile_program_from_source(
    const char *vertSource, const char *fragSource,
    const ShaderDefineCopy *defines, std::size_t defineCount) noexcept {
  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->create_program == nullptr)) {
    return kInvalidDeviceProgram;
  }

  std::unique_ptr<char[]> variantVertSource;
  std::unique_ptr<char[]> variantFragSource;
  const char *compiledVertSource = vertSource;
  const char *compiledFragSource = fragSource;
  if (defineCount > 0U) {
    variantVertSource =
        build_source_with_defines(vertSource, defines, defineCount);
    variantFragSource =
        build_source_with_defines(fragSource, defines, defineCount);
    if (!variantVertSource || !variantFragSource) {
      return kInvalidDeviceProgram;
    }
    compiledVertSource = variantVertSource.get();
    compiledFragSource = variantFragSource.get();
  }

  // The variant sources only need to outlive this call; naming the result
  // also keeps cppcheck from tying the returned handle to their lifetime.
  const DeviceProgramHandle program =
      dev->create_program(compiledVertSource, compiledFragSource);
  return program;
}

/// Builds the cooked shader binary path beside a source path (#138
/// Phase C): "<dir>/bgfx/cooked/<file>.<variantKey>.<profile>.bin",
/// where variantKey is the entry's define names sorted and joined with
/// '-' ("default" when none). False when a define carries a value (the
/// cook keys flag defines only) or the result would not fit.
bool build_cooked_shader_path(const char *sourcePath,
                              const ShaderDefineCopy *defines,
                              std::size_t defineCount, const char *profile,
                              char *out, std::size_t outSize) noexcept {
  const char *slash = std::strrchr(sourcePath, '/');
  const std::size_t dirLength =
      (slash != nullptr)
          ? static_cast<std::size_t>(slash - sourcePath) + 1U
          : 0U;
  const char *file = (slash != nullptr) ? (slash + 1U) : sourcePath;

  if (defineCount > kMaxShaderDefines) {
    return false;
  }
  const char *sorted[kMaxShaderDefines] = {};
  for (std::size_t i = 0U; i < defineCount; ++i) {
    if (std::strcmp(defines[i].value, kEnabledDefineValue) != 0) {
      return false;
    }
    sorted[i] = defines[i].name;
  }
  // Insertion sort: at most kMaxShaderDefines entries, and GCC 16's
  // -Werror=array-bounds false-positives on std::sort's inlined
  // introsort over the fixed-size array.
  for (std::size_t i = 1U; i < defineCount; ++i) {
    const char *current = sorted[i];
    std::size_t j = i;
    while ((j > 0U) && (std::strcmp(current, sorted[j - 1U]) < 0)) {
      sorted[j] = sorted[j - 1U];
      --j;
    }
    sorted[j] = current;
  }

  char key[kMaxShaderDefines * kMaxDefineNameLength] = {};
  if (defineCount == 0U) {
    core::copy_string(key, sizeof(key), "default");
  } else {
    std::size_t used = 0U;
    for (std::size_t i = 0U; i < defineCount; ++i) {
      const int written = std::snprintf(key + used, sizeof(key) - used,
                                        (i > 0U) ? "-%s" : "%s", sorted[i]);
      if (written < 0) {
        return false;
      }
      used += static_cast<std::size_t>(written);
      if (used >= sizeof(key)) {
        return false;
      }
    }
  }

  const int written =
      std::snprintf(out, outSize, "%.*sbgfx/cooked/%s.%s.%s.bin",
                    static_cast<int>(dirLength), sourcePath, file, key,
                    profile);
  return (written > 0) && (static_cast<std::size_t>(written) < outSize);
}

/// Reads one stage's cooked binary for a variant, falling back to the
/// "default" (no-define) cook when the flagged binary does not exist —
/// the manifest only cooks a variant for the stages its defines touch
/// (e.g. SKINNED changes the vertex stage; the fragment stage is the
/// default binary). *usedDefaultKey reports which cook satisfied the
/// read so the caller can pair introspection sidecars consistently.
bool read_cooked_stage(const char *sourcePath,
                       const ShaderDefineCopy *defines,
                       std::size_t defineCount, const char *profile,
                       void **outData, std::size_t *outSize,
                       bool *usedDefaultKey) noexcept {
  char path[kMaxPathLength * 2U] = {};
  if (build_cooked_shader_path(sourcePath, defines, defineCount, profile,
                               path, sizeof(path)) &&
      core::vfs_read_binary(path, outData, outSize)) {
    *usedDefaultKey = (defineCount == 0U);
    return true;
  }
  if (defineCount == 0U) {
    return false;
  }
  if (build_cooked_shader_path(sourcePath, nullptr, 0U, profile, path,
                               sizeof(path)) &&
      core::vfs_read_binary(path, outData, outSize)) {
    *usedDefaultKey = true;
    return true;
  }
  return false;
}

/// Cooked-binary program path (#138 Phase C): loads both stages'
/// cooked binaries for the backend's profile through the VFS and links
/// them via create_program_binary. Invalid when the backend does not
/// consume cooked programs or a binary is missing (the caller falls
/// back to source compilation).
DeviceProgramHandle try_cooked_program(const char *vertPath,
                                       const char *fragPath,
                                       const ShaderDefineCopy *defines,
                                       std::size_t defineCount) noexcept {
  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || !dev->caps.cookedPrograms ||
      (dev->create_program_binary == nullptr) ||
      (dev->cooked_program_profile == nullptr)) {
    return kInvalidDeviceProgram;
  }
  const char *profile = dev->cooked_program_profile();
  bool vertUsedDefault = false;
  bool fragUsedDefault = false;
  void *vertData = nullptr;
  std::size_t vertSize = 0U;
  if (!read_cooked_stage(vertPath, defines, defineCount, profile, &vertData,
                         &vertSize, &vertUsedDefault)) {
    return kInvalidDeviceProgram;
  }
  void *fragData = nullptr;
  std::size_t fragSize = 0U;
  if (!read_cooked_stage(fragPath, defines, defineCount, profile, &fragData,
                         &fragSize, &fragUsedDefault)) {
    core::vfs_free(vertData);
    return kInvalidDeviceProgram;
  }

  // GLSL-family binaries embed no uniform table; hand the spirv
  // siblings to the backend as introspection sidecars (#138).
  const bool glslFamily = (std::strcmp(profile, "glsl") == 0) ||
                          (std::strcmp(profile, "essl") == 0);
  DeviceProgramHandle program = kInvalidDeviceProgram;
  if (glslFamily && (dev->create_program_binary_introspected != nullptr)) {
    char vertMetaPath[kMaxPathLength * 2U] = {};
    char fragMetaPath[kMaxPathLength * 2U] = {};
    void *vertMeta = nullptr;
    void *fragMeta = nullptr;
    std::size_t vertMetaSize = 0U;
    std::size_t fragMetaSize = 0U;
    // Each sidecar pairs with the cook that satisfied its stage: a
    // stage that fell back to the default binary takes the default
    // spirv table.
    const std::size_t vertMetaDefines = vertUsedDefault ? 0U : defineCount;
    const std::size_t fragMetaDefines = fragUsedDefault ? 0U : defineCount;
    if (build_cooked_shader_path(vertPath, defines, vertMetaDefines, "spirv",
                                 vertMetaPath, sizeof(vertMetaPath)) &&
        build_cooked_shader_path(fragPath, defines, fragMetaDefines, "spirv",
                                 fragMetaPath, sizeof(fragMetaPath)) &&
        core::vfs_read_binary(vertMetaPath, &vertMeta, &vertMetaSize)) {
      if (!core::vfs_read_binary(fragMetaPath, &fragMeta, &fragMetaSize)) {
        core::vfs_free(vertMeta);
        vertMeta = nullptr;
      }
    }
    program = dev->create_program_binary_introspected(
        vertData, static_cast<std::ptrdiff_t>(vertSize), fragData,
        static_cast<std::ptrdiff_t>(fragSize), vertMeta,
        static_cast<std::ptrdiff_t>(vertMetaSize), fragMeta,
        static_cast<std::ptrdiff_t>(fragMetaSize));
    core::vfs_free(vertMeta);
    core::vfs_free(fragMeta);
  } else {
    program = dev->create_program_binary(
        vertData, static_cast<std::ptrdiff_t>(vertSize), fragData,
        static_cast<std::ptrdiff_t>(fragSize));
  }
  core::vfs_free(vertData);
  core::vfs_free(fragData);
  return program;
}

/// Logs one shader diagnostic in the `<path>: <reason>` shape the editor
/// console parses for its Open/Select navigation actions (#217).
void log_shader_path_error(const char *path, const char *reason) noexcept {
  char message[640] = {};
  std::snprintf(message, sizeof(message), "%s: %s",
                (path != nullptr) ? path : "(null)", reason);
  core::log_message(core::LogLevel::Error, "shader", message);
}

bool try_reload_entry(ShaderEntry &entry) noexcept {
  // Cooked binaries win when the backend consumes them; a missing or
  // unreadable binary falls back to source compilation below.
  const DeviceProgramHandle cooked = try_cooked_program(
      entry.vertPath, entry.fragPath, entry.defines, entry.defineCount);
  if (cooked != kInvalidDeviceProgram) {
    const RenderDevice *dev = render_device();
    if ((dev != nullptr) && (dev->destroy_program != nullptr) &&
        (entry.deviceProgram != kInvalidDeviceProgram)) {
      dev->destroy_program(entry.deviceProgram);
    }
    entry.deviceProgram = cooked;
    entry.vertMtime = core::vfs_file_mtime(entry.vertPath);
    entry.fragMtime = core::vfs_file_mtime(entry.fragPath);
    ++g_reloadEpoch;
    return true;
  }

  char *vertSource = nullptr;
  std::size_t vertSize = 0U;
  if (!core::vfs_read_text(entry.vertPath, &vertSource, &vertSize)) {
    log_shader_path_error(entry.vertPath, "failed to read vertex shader");
    return false;
  }

  char *fragSource = nullptr;
  std::size_t fragSize = 0U;
  if (!core::vfs_read_text(entry.fragPath, &fragSource, &fragSize)) {
    core::vfs_free(vertSource);
    log_shader_path_error(entry.fragPath, "failed to read fragment shader");
    return false;
  }

  const DeviceProgramHandle newProgram =
      compile_program_from_source(vertSource, fragSource, entry.defines,
                                  entry.defineCount);
  core::vfs_free(vertSource);
  core::vfs_free(fragSource);

  if (newProgram == kInvalidDeviceProgram) {
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "%s %s: shader compilation failed — keeping old program",
                  entry.vertPath, entry.fragPath);
    core::log_message(core::LogLevel::Error, "shader", message);
    return false;
  }

  const RenderDevice *dev = render_device();
  if ((dev != nullptr) && (dev->destroy_program != nullptr) &&
      (entry.deviceProgram != kInvalidDeviceProgram)) {
    dev->destroy_program(entry.deviceProgram);
  }

  entry.deviceProgram = newProgram;
  entry.vertMtime = core::vfs_file_mtime(entry.vertPath);
  entry.fragMtime = core::vfs_file_mtime(entry.fragPath);
  ++g_reloadEpoch;
  return true;
}

/// Loads a shader program entry with an optional copied define set.
ShaderProgramHandle load_shader_program_internal(
    const char *vertPath, const char *fragPath, const ShaderDefine *defines,
    std::size_t defineCount) noexcept {
  std::size_t slot = kMaxShaderPrograms;
  for (std::size_t i = 0U; i < kMaxShaderPrograms; ++i) {
    if (!g_entries[i].active) {
      slot = i;
      break;
    }
  }

  if (slot == kMaxShaderPrograms) {
    log_shader_path_error(vertPath, "shader program registry full");
    return kInvalidShaderProgram;
  }

  ShaderEntry &entry = g_entries[slot];
  const std::uint32_t generation = entry.generation;
  entry = ShaderEntry{};
  entry.generation = generation;
  safe_copy_path(entry.vertPath, vertPath);
  safe_copy_path(entry.fragPath, fragPath);
  if (defineCount > 0U) {
    copy_shader_defines(entry, defines, defineCount);
  }
  entry.active = true;

  if (!try_reload_entry(entry)) {
    reset_shader_entry(slot);
    return kInvalidShaderProgram;
  }

  return ShaderProgramHandle{static_cast<std::uint32_t>(slot + 1U),
                             entry.generation};
}

} // namespace

/// Initializes the owning system for shader system.
bool initialize_shader_system() noexcept {
  if (g_initialized) {
    return true;
  }

  for (std::size_t i = 0U; i < kMaxShaderPrograms; ++i) {
    reset_shader_entry(i);
  }
  for (auto &variant : g_variants) {
    variant = ShaderVariantEntry{};
  }

  g_initialized = true;
  return true;
}

/// Shuts down the owning system for shader system.
void shutdown_shader_system() noexcept {
  if (!g_initialized) {
    return;
  }

  const RenderDevice *dev = render_device();
  for (std::size_t i = 0U; i < kMaxShaderPrograms; ++i) {
    ShaderEntry &e = g_entries[i];
    if (e.active && (e.deviceProgram != kInvalidDeviceProgram) &&
        (dev != nullptr) && (dev->destroy_program != nullptr)) {
      dev->destroy_program(e.deviceProgram);
    }
    reset_shader_entry(i);
  }
  for (auto &variant : g_variants) {
    variant = ShaderVariantEntry{};
  }

  g_initialized = false;
}

/// Loads the requested resource for shader program.
ShaderProgramHandle load_shader_program(const char *vertPath,
                                        const char *fragPath) noexcept {
  if ((vertPath == nullptr) || (fragPath == nullptr) || !g_initialized ||
      !string_fits(vertPath, kMaxPathLength) ||
      !string_fits(fragPath, kMaxPathLength)) {
    return kInvalidShaderProgram;
  }

  return load_shader_program_internal(vertPath, fragPath, nullptr, 0U);
}

/// Computes a stable key for a shader path pair and unordered define set.
ShaderVariantKey shader_variant_key(const ShaderVariantDesc &desc) noexcept {
  if (!validate_shader_variant_desc(desc)) {
    return ShaderVariantKey{};
  }

  std::array<const ShaderDefine *, kMaxShaderDefines> sorted{};
  for (std::size_t i = 0U; i < desc.defineCount; ++i) {
    sorted[i] = &desc.defines[i];
  }
  sort_shader_defines(sorted, desc.defineCount);

  std::uint64_t hash = core::kFnv1a64Offset;
  hash_string(hash, desc.vertPath);
  hash_string(hash, desc.fragPath);
  for (std::size_t i = 0U; i < desc.defineCount; ++i) {
    hash_string(hash, sorted[i]->name);
    hash_string(hash, normalized_define_value(*sorted[i]));
  }

  return ShaderVariantKey{hash != 0U ? hash : 1U};
}

/// Loads a shader variant, compiling and caching it when the key is absent.
ShaderProgramHandle load_shader_variant(const ShaderVariantDesc &desc) noexcept {
  if (!g_initialized) {
    return kInvalidShaderProgram;
  }

  const ShaderVariantKey key = shader_variant_key(desc);
  if (key.value == 0U) {
    return kInvalidShaderProgram;
  }

  for (const auto &variant : g_variants) {
    if (variant.active && (variant.key == key)) {
      return variant.handle;
    }
  }

  std::size_t variantSlot = kMaxShaderVariants;
  for (std::size_t i = 0U; i < kMaxShaderVariants; ++i) {
    if (!g_variants[i].active) {
      variantSlot = i;
      break;
    }
  }
  if (variantSlot == kMaxShaderVariants) {
    log_shader_path_error(desc.vertPath, "shader variant cache full");
    return kInvalidShaderProgram;
  }

  const ShaderProgramHandle handle = load_shader_program_internal(
      desc.vertPath, desc.fragPath, desc.defines, desc.defineCount);
  if (handle == kInvalidShaderProgram) {
    return kInvalidShaderProgram;
  }

  g_variants[variantSlot].active = true;
  g_variants[variantSlot].key = key;
  g_variants[variantSlot].handle = handle;
  return handle;
}

/// Selects shader defines required by a material and mesh skinning state.
MaterialShaderVariantSelection select_material_shader_defines(
    const Material &material, bool skinned) noexcept {
  MaterialShaderVariantSelection selection{};

  if (material.albedoTexture != kInvalidTextureHandle) {
    append_material_shader_define(selection, "HAS_ALBEDO_TEXTURE");
  }
  if (material.normalTexture != kInvalidTextureHandle) {
    append_material_shader_define(selection, "HAS_NORMAL_MAP");
  }
  if (has_emissive_value(material.emissive)) {
    append_material_shader_define(selection, "HAS_EMISSIVE");
  }
  if (material.opacity < 1.0F) {
    append_material_shader_define(selection, "MATERIAL_TRANSLUCENT");
  }
  if (skinned) {
    append_material_shader_define(selection, "SKINNED");
  }

  return selection;
}

/// Destroys or releases the requested object, handle, or resource for shader program.
void destroy_shader_program(ShaderProgramHandle handle) noexcept {
  if (!is_current_handle(handle)) {
    return;
  }

  ShaderEntry &entry = g_entries[handle.id - 1U];
  if (!entry.active) {
    return;
  }

  const RenderDevice *dev = render_device();
  if ((dev != nullptr) && (dev->destroy_program != nullptr) &&
      (entry.deviceProgram != kInvalidDeviceProgram)) {
    dev->destroy_program(entry.deviceProgram);
  }

  remove_variants_for_handle(handle);
  reset_shader_entry(handle.id - 1U);
}

DeviceProgramHandle shader_device_program(ShaderProgramHandle handle) noexcept {
  if (!is_current_handle(handle)) {
    return kInvalidDeviceProgram;
  }

  const ShaderEntry &entry = g_entries[handle.id - 1U];
  return entry.deviceProgram;
}

std::uint64_t shader_reload_epoch() noexcept { return g_reloadEpoch; }

void check_shader_reload() noexcept {
  if (!g_initialized) {
    return;
  }

  for (auto &entry : g_entries) {
    if (!entry.active) {
      continue;
    }

    const std::int64_t vertMtime = core::vfs_file_mtime(entry.vertPath);
    const std::int64_t fragMtime = core::vfs_file_mtime(entry.fragPath);

    if ((vertMtime != entry.vertMtime) || (fragMtime != entry.fragMtime)) {
      char message[640] = {};
      std::snprintf(message, sizeof(message), "%s %s: reloading modified shader",
                    entry.vertPath, entry.fragPath);
      core::log_message(core::LogLevel::Info, "shader", message);
      try_reload_entry(entry);
    }
  }
}

} // namespace engine::renderer
