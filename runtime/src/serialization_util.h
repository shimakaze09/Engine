// Shared file-IO and JSON field helpers for the runtime serializers.
// scene_serializer.cpp and prefab_serializer.cpp previously kept diverging
// private copies of these (REVIEW_FINDINGS S5); this module-internal header
// is now the single implementation both compile against.

#pragma once

#include <cstddef>
#include <cstdio>
#include <memory>

#include "engine/core/json.h"
#include "engine/math/quat.h"
#include "engine/math/vec2.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

// --- Whole-file IO ---------------------------------------------------------

/// Opens `path` for binary reading; false on null args or open failure.
bool open_file_for_read(const char *path, FILE **outFile) noexcept;
/// Opens `path` for binary writing (truncates); false on null args or failure.
bool open_file_for_write(const char *path, FILE **outFile) noexcept;
/// Reads an entire file into a null-terminated heap buffer.
/// Fails on empty files, short reads, or allocation failure.
bool read_text_file(const char *path, std::unique_ptr<char[]> *outBuffer,
                    std::size_t *outSize) noexcept;
/// Writes `size` bytes to `path`; false unless every byte lands.
bool write_text_file(const char *path, const char *text,
                     std::size_t size) noexcept;

// --- Vector / quaternion JSON fields (fixed-size float arrays) -------------

/// Writes a Vec2 as a 2-element float array under `key`.
void write_vec2(core::JsonWriter &writer, const char *key,
                const math::Vec2 &value) noexcept;
/// Writes a Vec3 as a 3-element float array under `key`.
void write_vec3(core::JsonWriter &writer, const char *key,
                const math::Vec3 &value) noexcept;
/// Writes a Vec4 as a 4-element float array under `key`.
void write_vec4(core::JsonWriter &writer, const char *key,
                const math::Vec4 &value) noexcept;
/// Writes a Quat as a 4-element float array (x, y, z, w) under `key`.
void write_quat(core::JsonWriter &writer, const char *key,
                const math::Quat &value) noexcept;

/// Reads exactly `expectedCount` floats from a JSON array; the element count
/// must match exactly (serializers always write exact-size arrays).
bool read_float_array(const core::JsonParser &parser,
                      const core::JsonValue &arrayValue, float *outValues,
                      std::size_t expectedCount) noexcept;
/// Reads a Vec2 from a 2-element float array.
bool read_vec2(const core::JsonParser &parser, const core::JsonValue &value,
               math::Vec2 *outVec) noexcept;
/// Reads a Vec3 from a 3-element float array.
bool read_vec3(const core::JsonParser &parser, const core::JsonValue &value,
               math::Vec3 *outVec) noexcept;
/// Reads a Vec4 from a 4-element float array.
bool read_vec4(const core::JsonParser &parser, const core::JsonValue &value,
               math::Vec4 *outVec) noexcept;
/// Reads a Quat from a 4-element float array (x, y, z, w).
bool read_quat(const core::JsonParser &parser, const core::JsonValue &value,
               math::Quat *outQuat) noexcept;

// --- FoliagePatchComponent -------------------------------------------------

/// Writes the full foliage patch object (LOD mesh ids, material, wind, and
/// the clamped instance array) under kJsonKeyFoliagePatchComponent.
void write_foliage_patch_component(
    core::JsonWriter &writer, const FoliagePatchComponent &component) noexcept;
/// Reads a foliage patch object. Strict: any present-but-malformed field
/// fails the read (missing fields keep component defaults). Instance counts
/// are clamped to kMaxInstances and the serialized instanceCount.
bool read_foliage_patch_component(const core::JsonParser &parser,
                                  const core::JsonValue &foliageObject,
                                  FoliagePatchComponent *outComponent) noexcept;

} // namespace engine::runtime
