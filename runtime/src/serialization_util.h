// Shared file-IO and JSON field helpers for the runtime serializers.
// scene_serializer.cpp and prefab_serializer.cpp previously kept diverging
// private copies of these (REVIEW_FINDINGS S5); this module-internal header
// is now the single implementation both compile against.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "engine/core/json.h"
#include "engine/core/reflect.h"
#include "engine/math/quat.h"
#include "engine/math/vec2.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

// --- Whole-file IO ---------------------------------------------------------

/// Opens `path` for binary reading; false on null args or open failure.
/// Deliberately read-only: every production write goes through
/// write_text_file's atomic replacement, so this header offers no
/// truncating-open counterpart for a caller to bypass it with.
bool open_file_for_read(const char *path, FILE **outFile) noexcept;
/// Reads an entire file into a null-terminated heap buffer.
/// Fails on empty files, short reads, or allocation failure.
bool read_text_file(const char *path, std::unique_ptr<char[]> *outBuffer,
                    std::size_t *outSize) noexcept;
/// Writes `size` bytes to `path`; false unless every byte lands.
bool write_text_file(const char *path, const char *text,
                     std::size_t size) noexcept;

// --- Schema version --------------------------------------------------------

/// JSON key naming the schema revision a document was written against.
/// Both runtime serializers write it and gate their reads on it.
inline constexpr const char *kSchemaVersionKey = "version";

/// Whether a document's schema revision is one this build can interpret.
/// A document that omits the key reads as revision 1, the earliest revision
/// of either runtime format, so a hand-authored document without it still
/// loads; a present key must hold an unsigned integer in
/// [1, currentVersion]. `noun` names the document in the diagnostics logged
/// on `channel`. Callers check this before consuming any payload, so a
/// document written by a newer engine leaves the destination untouched
/// instead of loading as a partial, resavable reduction of itself.
bool schema_version_supported(const core::JsonParser &parser,
                              const core::JsonValue &root,
                              std::uint32_t currentVersion, const char *noun,
                              const char *channel) noexcept;

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

// --- Reflection-backed component codecs ------------------------------------

/// Reflection descriptors for the component types both serializers encode
/// through core reflection metadata (registered in reflect_types.cpp).
struct ReflectedComponentDescriptors final {
  const core::TypeDescriptor *transform = nullptr;
  const core::TypeDescriptor *rigidBody = nullptr;
  const core::TypeDescriptor *springArm = nullptr;
  const core::TypeDescriptor *reflectionProbe = nullptr;
  const core::TypeDescriptor *pointLight = nullptr;
  const core::TypeDescriptor *spotLight = nullptr;
  const core::TypeDescriptor *sceneCapture = nullptr;
  const core::TypeDescriptor *camera = nullptr;
};

/// Reflection descriptor for a reflected component type (one overload per
/// type; a new reflected row without one fails to compile).
inline const core::TypeDescriptor &
component_descriptor(const ReflectedComponentDescriptors &descs,
                     const Transform *) noexcept {
  return *descs.transform;
}
/// RigidBody descriptor selector.
inline const core::TypeDescriptor &
component_descriptor(const ReflectedComponentDescriptors &descs,
                     const RigidBody *) noexcept {
  return *descs.rigidBody;
}
/// SpringArm descriptor selector.
inline const core::TypeDescriptor &
component_descriptor(const ReflectedComponentDescriptors &descs,
                     const SpringArmComponent *) noexcept {
  return *descs.springArm;
}
/// ReflectionProbe descriptor selector.
inline const core::TypeDescriptor &
component_descriptor(const ReflectedComponentDescriptors &descs,
                     const ReflectionProbeComponent *) noexcept {
  return *descs.reflectionProbe;
}
/// PointLight descriptor selector.
inline const core::TypeDescriptor &
component_descriptor(const ReflectedComponentDescriptors &descs,
                     const PointLightComponent *) noexcept {
  return *descs.pointLight;
}
/// SpotLight descriptor selector.
inline const core::TypeDescriptor &
component_descriptor(const ReflectedComponentDescriptors &descs,
                     const SpotLightComponent *) noexcept {
  return *descs.spotLight;
}
/// SceneCapture descriptor selector.
inline const core::TypeDescriptor &
component_descriptor(const ReflectedComponentDescriptors &descs,
                     const SceneCaptureComponent *) noexcept {
  return *descs.sceneCapture;
}
/// Camera descriptor selector.
inline const core::TypeDescriptor &
component_descriptor(const ReflectedComponentDescriptors &descs,
                     const CameraComponent *) noexcept {
  return *descs.camera;
}

/// Looks up every reflected component descriptor/// Looks up every reflected component descriptor/// Looks up every reflected component descriptor; logs under `logChannel`
/// and fails when any registration is missing.
bool find_reflected_component_descriptors(
    ReflectedComponentDescriptors *outDescs, const char *logChannel) noexcept;

/// Writes `instance` as a JSON object keyed `componentName`, one entry per
/// reflection field descriptor.
bool write_reflected_component(core::JsonWriter &writer,
                               const char *componentName,
                               const core::TypeDescriptor &descriptor,
                               const void *instance) noexcept;
/// Reads reflected fields into `instance`; missing fields keep the caller's
/// defaults, present-but-malformed fields fail the read.
bool read_reflected_component(const core::JsonParser &parser,
                              const core::JsonValue &componentObject,
                              const core::TypeDescriptor &descriptor,
                              void *instance) noexcept;

// --- MeshComponent / LightComponent ----------------------------------------

/// Writes the mesh component under kJsonKeyMeshComponent (material/capture
/// ids only when set, keeping pre-feature files byte-identical).
void write_mesh_component(core::JsonWriter &writer,
                          const MeshComponent &component) noexcept;
/// Reads a mesh component, including the legacy "meshId" fallback for
/// content authored before asset ids.
bool read_mesh_component(const core::JsonParser &parser,
                         const core::JsonValue &meshObject,
                         MeshComponent *outComponent) noexcept;
/// Writes the light component under kJsonKeyLightComponent.
void write_light_component(core::JsonWriter &writer,
                           const LightComponent &component) noexcept;
/// Reads a light component; `type` clamps to a valid LightType on load.
bool read_light_component(const core::JsonParser &parser,
                          const core::JsonValue &lightObject,
                          LightComponent *outComponent) noexcept;

// --- Collider -------------------------------------------------------------

/// Writes every collider field, including its validated shape and local pose.
bool write_collider_component(core::JsonWriter &writer,
                              const Collider &component) noexcept;
/// Reads a collider strictly while retaining defaults for omitted legacy data.
bool read_collider_component(const core::JsonParser &parser,
                             const core::JsonValue &colliderObject,
                             Collider *outComponent) noexcept;

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

// --- AnimationComponent ----------------------------------------------------

/// Writes the animation component under `key`, carrying every authored
/// field (issue #253). A component whose `playing`/`playbackSpeed` still
/// hold their defaults writes the bare controller-path string the format
/// has always used, so files that author no non-default value stay
/// byte-identical; only a component that the string shape cannot represent
/// widens to an object. An empty controller path writes nothing, matching
/// the pre-registry writer. Runtime state (slots, current/previous state,
/// blend timers, parameters) is never serialized.
void write_animation_component(core::JsonWriter &writer, const char *key,
                               const AnimationComponent &component) noexcept;
/// Reads an animation component from either shape: a bare string is the
/// legacy/default form and supplies the controller path with defaults for
/// the remaining authored fields; an object reads `controllerPath` plus the
/// optional `playing` and `playbackSpeed`. Strict: a present-but-malformed
/// field fails the read (absent fields keep component defaults). The
/// controller path is required by both shapes; `requireNonEmptyPath`
/// additionally rejects an empty one, which the prefab format has always
/// done.
bool read_animation_component(const core::JsonParser &parser,
                              const core::JsonValue &value,
                              bool requireNonEmptyPath,
                              AnimationComponent *outComponent) noexcept;

} // namespace engine::runtime
