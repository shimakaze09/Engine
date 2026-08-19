// Implements the editor Inspector's semantic field/component metadata
// tables and the pure Euler/layer-name helpers declared in
// editor_inspector_metadata.h.

#include "editor_inspector_metadata.h"

#include <cstring>

namespace engine::editor {

namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kRadToDeg = 180.0F / kPi;
constexpr float kDegToRad = kPi / 180.0F;

// Enum label tables for the two current Enum-widget fields; only the
// analytically authorable Collider shapes are offered (a convex hull needs
// primitive-spawn provenance and heightfields are not editor-authorable at
// all, so switching a live collider into either would create a payload-
// less shape) -- matches the pre-existing custom combo's authorable count.
constexpr const char *kColliderShapeLabels[] = {"Box", "Sphere", "Capsule"};
constexpr const char *kLightTypeLabels[] = {"Directional", "Point"};
constexpr const char *kCameraProjectionLabels[] = {"Perspective",
                                                    "Orthographic"};

// One row per annotated field. Fields with no row here still draw through
// the generic Auto path -- omission never hides a field.
constexpr FieldMetadata kFieldMetadataTable[] = {
    {"engine::runtime::Collider", "shape", "Shape", "Shape",
     "Analytic collider shapes only; convex hull and heightfield shapes "
     "are set by primitive spawn/import, not this combo.", nullptr, 0.0F,
     0.0F, 0.0F, InspectorWidget::Enum, false, false, kColliderShapeLabels,
     3U},
    {"engine::runtime::LightComponent", "type", "Type", "Light", nullptr,
     nullptr, 0.0F, 0.0F, 0.0F, InspectorWidget::Enum, false, false,
     kLightTypeLabels, 2U},
    {"engine::runtime::Transform", "position", "Position", "Transform",
     "World-relative position of the object's local origin.", "m", 0.05F,
     0.0F, 0.0F, InspectorWidget::Drag, false, false},
    {"engine::runtime::Transform", "rotation", "Rotation", "Transform",
     "Orientation in degrees (pitch, yaw, roll). Stored internally as a "
     "normalized quaternion; degrees are a display projection recomputed "
     "each frame, never the source of truth.",
     "deg", 1.0F, 0.0F, 0.0F, InspectorWidget::EulerDegrees, false, false},
    {"engine::runtime::Transform", "scale", "Scale", "Transform",
     "Non-uniform local scale multiplier.", nullptr, 0.02F, 0.0F, 0.0F,
     InspectorWidget::Drag, false, false},
    {"engine::runtime::Transform", "parentId", "Parent Id", "Transform",
     "Internal persistent id of the parent transform; use Reparent in the "
     "Hierarchy panel instead of editing this directly.", nullptr, 0.0F,
     0.0F, 0.0F, InspectorWidget::Auto, true, true},

    {"engine::runtime::RigidBody", "inverseMass", "Inverse Mass", "Physics",
     "1 / mass in kg^-1. 0 means infinite mass (static or kinematic).",
     "1/kg", 0.01F, 0.0F, 1000.0F, InspectorWidget::Drag, false, false},
    {"engine::runtime::RigidBody", "inverseInertia", "Inverse Inertia",
     "Physics", "1 / rotational inertia about the body's principal axis.",
     nullptr, 0.01F, 0.0F, 1000.0F, InspectorWidget::Drag, false, false},
    {"engine::runtime::RigidBody", "velocity", "Velocity", "Physics",
     "Linear velocity in m/s.", "m/s", 0.05F, 0.0F, 0.0F,
     InspectorWidget::Drag, true, false},
    {"engine::runtime::RigidBody", "acceleration", "Acceleration", "Physics",
     "Accumulated linear acceleration for the current step.", "m/s^2", 0.05F,
     0.0F, 0.0F, InspectorWidget::Drag, true, false},
    {"engine::runtime::RigidBody", "angularVelocity", "Angular Velocity",
     "Physics", "Angular velocity in rad/s.", "rad/s", 0.05F, 0.0F, 0.0F,
     InspectorWidget::Drag, true, false},
    {"engine::runtime::RigidBody", "sleeping", "Sleeping", "Physics",
     "Simulation runtime state, not authored data.", nullptr, 0.0F, 0.0F,
     0.0F, InspectorWidget::Auto, true, false},

    {"engine::runtime::Collider", "restitution", "Bounciness",
     "Physics Material", "Fraction of impact speed returned on collision "
     "(0 = no bounce, 1 = perfectly elastic).", nullptr, 0.0F, 0.0F, 1.0F,
     InspectorWidget::Slider, false, false},
    {"engine::runtime::Collider", "staticFriction", "Static Friction",
     "Physics Material", "Friction coefficient resisting the start of "
     "relative sliding.", nullptr, 0.0F, 0.0F, 2.0F, InspectorWidget::Slider,
     false, false},
    {"engine::runtime::Collider", "dynamicFriction", "Dynamic Friction",
     "Physics Material", "Friction coefficient resisting ongoing relative "
     "sliding.", nullptr, 0.0F, 0.0F, 2.0F, InspectorWidget::Slider, false,
     false},
    {"engine::runtime::Collider", "density", "Density", "Physics Material",
     "Mass per unit volume, used to derive body mass from this shape.",
     "kg/m^3", 0.05F, 0.0F, 0.0F, InspectorWidget::Drag, false, false},
    {"engine::runtime::Collider", "collisionLayer", "Collision Layer",
     "Collision", "Layers this collider belongs to.", nullptr, 0.0F, 0.0F,
     0.0F, InspectorWidget::LayerMask, false, false},
    {"engine::runtime::Collider", "collisionMask", "Collides With",
     "Collision", "Layers this collider is tested against.", nullptr, 0.0F,
     0.0F, 0.0F, InspectorWidget::LayerMask, false, false},
    {"engine::runtime::Collider", "localPosition", "Local Position", "Shape",
     nullptr, "m", 0.02F, 0.0F, 0.0F, InspectorWidget::Drag, false, false},
    {"engine::runtime::Collider", "halfExtents", "Half Extents", "Shape",
     "Meaning depends on Shape: box half-size, sphere/capsule radius in x, "
     "or hull bounds.", "m", 0.02F, 0.0F, 0.0F, InspectorWidget::Drag, false,
     false},

    {"engine::runtime::PointLightComponent", "color", "Color", "Light",
     nullptr, nullptr, 0.0F, 0.0F, 0.0F, InspectorWidget::Color, false,
     false},
    {"engine::runtime::PointLightComponent", "radius", "Range", "Light",
     "Distance in meters at which the light's contribution reaches zero.",
     "m", 0.05F, 0.0F, 0.0F, InspectorWidget::Drag, false, false},

    {"engine::runtime::SpotLightComponent", "color", "Color", "Light",
     nullptr, nullptr, 0.0F, 0.0F, 0.0F, InspectorWidget::Color, false,
     false},
    {"engine::runtime::SpotLightComponent", "radius", "Range", "Light",
     "Distance in meters at which the light's contribution reaches zero.",
     "m", 0.05F, 0.0F, 0.0F, InspectorWidget::Drag, false, false},
    {"engine::runtime::SpotLightComponent", "innerConeAngle", "Inner Cone",
     "Light", "Full-intensity cone half-angle.", "deg", 1.0F, 0.0F, 90.0F,
     InspectorWidget::AngleDegrees, false, false},
    {"engine::runtime::SpotLightComponent", "outerConeAngle", "Outer Cone",
     "Light", "Falloff cone half-angle; must be >= Inner Cone.", "deg", 1.0F,
     0.0F, 90.0F, InspectorWidget::AngleDegrees, false, false},

    {"engine::runtime::LightComponent", "color", "Color", "Light", nullptr,
     nullptr, 0.0F, 0.0F, 0.0F, InspectorWidget::Color, false, false},

    {"engine::runtime::ReflectionProbeComponent", "prefilteredResolution",
     "Prefiltered Resolution", "Bake", "Cubemap face size for the specular "
     "prefilter chain.", "px", 0.0F, 0.0F, 0.0F, InspectorWidget::Auto, true,
     false},
    {"engine::runtime::ReflectionProbeComponent", "irradianceResolution",
     "Irradiance Resolution", "Bake", "Cubemap face size for the diffuse "
     "irradiance map.", "px", 0.0F, 0.0F, 0.0F, InspectorWidget::Auto, true,
     false},
    {"engine::runtime::ReflectionProbeComponent", "brdfLutResolution",
     "BRDF LUT Resolution", "Bake", nullptr, "px", 0.0F, 0.0F, 0.0F,
     InspectorWidget::Auto, true, false},
    {"engine::runtime::ReflectionProbeComponent", "mipLevels", "Mip Levels",
     "Bake", nullptr, nullptr, 0.0F, 0.0F, 0.0F, InspectorWidget::Auto, true,
     false},

    {"engine::runtime::SceneCaptureComponent", "fovRadians",
     "Field of View", "Capture", nullptr, "deg", 1.0F, 1.0F, 179.0F,
     InspectorWidget::AngleDegrees, false, false},
    {"engine::runtime::SceneCaptureComponent", "nearPlane", "Near Plane",
     "Capture", nullptr, "m", 0.01F, 0.001F, 0.0F, InspectorWidget::Drag,
     false, false},
    {"engine::runtime::SceneCaptureComponent", "farPlane", "Far Plane",
     "Capture", nullptr, "m", 0.5F, 0.001F, 0.0F, InspectorWidget::Drag,
     false, false},

    {"engine::runtime::CameraComponent", "projection", "Projection",
     "Camera", "Perspective renders with field-of-view depth; Orthographic "
     "renders parallel-projected using Ortho Size as the half-height "
     "(the sky keeps perspective directional sampling).", nullptr, 0.0F, 0.0F, 0.0F,
     InspectorWidget::Enum, false, false, kCameraProjectionLabels, 2U},
    {"engine::runtime::CameraComponent", "fovRadians", "Field of View",
     "Camera", nullptr, "deg", 1.0F, 1.0F, 179.0F,
     InspectorWidget::AngleDegrees, false, false},
    {"engine::runtime::CameraComponent", "orthographicSize",
     "Orthographic Size", "Camera",
     "Half-height of the orthographic view volume, in world units.", "m",
     0.1F, 0.01F, 0.0F, InspectorWidget::Drag, false, false},
    {"engine::runtime::CameraComponent", "nearPlane", "Near Plane", "Camera",
     nullptr, "m", 0.01F, 0.001F, 0.0F, InspectorWidget::Drag, false, false},
    {"engine::runtime::CameraComponent", "farPlane", "Far Plane", "Camera",
     nullptr, "m", 0.5F, 0.001F, 0.0F, InspectorWidget::Drag, false, false},
    {"engine::runtime::CameraComponent", "priority", "Priority", "Camera",
     "Highest active priority wins CameraManager's selection; ties resolve "
     "by authoring order.", nullptr, 0.1F, 0.0F, 0.0F, InspectorWidget::Drag,
     false, false},
    {"engine::runtime::CameraComponent", "blendSpeed", "Blend Speed",
     "Camera", "How fast CameraManager blends toward this camera once it "
     "becomes active.", nullptr, 0.1F, 0.0F, 0.0F, InspectorWidget::Drag,
     false, false},
};

constexpr ComponentMetadata kComponentMetadataTable[] = {
    {"engine::runtime::Transform", "Transform", "Core",
     "Position, rotation, and scale. Every scene object owns one."},
    {"engine::runtime::NameComponent", "Name", "Core",
     "Display name. Every scene object owns one."},
    {"engine::runtime::RigidBody", "Rigid Body", "Physics",
     "Makes the object simulate under physics (gravity, forces, impacts)."},
    {"engine::runtime::Collider", "Collider", "Physics",
     "Defines the object's collision shape and physical material."},
    {"engine::runtime::MeshComponent", "Mesh", "Rendering",
     "Renders a mesh asset with a material."},
    {"engine::runtime::LightComponent", "Directional/Point Light",
     "Rendering", "Legacy combined light; prefer Point Light or Spot "
     "Light for new objects."},
    {"engine::runtime::PointLightComponent", "Point Light", "Rendering",
     "Omnidirectional light with a falloff range."},
    {"engine::runtime::SpotLightComponent", "Spot Light", "Rendering",
     "Cone-shaped light with inner/outer falloff angles."},
    {"engine::runtime::ReflectionProbeComponent", "Reflection Probe",
     "Rendering", "Bakes an environment cubemap for local reflections."},
    {"engine::runtime::SceneCaptureComponent", "Scene Capture", "Rendering",
     "Renders the scene from this object's transform into a texture."},
    {"engine::runtime::FoliagePatchComponent", "Foliage Patch", "Rendering",
     "Instanced foliage placement with wind and LODs."},
    {"engine::runtime::SpringArmComponent", "Spring Arm", "Gameplay",
     "Camera boom that resolves collisions between its owner and a target "
     "length."},
    {"engine::runtime::ScriptComponent", "Script", "Gameplay",
     "Attaches a Lua behaviour script to this object."},
    {"engine::runtime::AnimationComponent", "Animation", "Gameplay",
     "Drives a skeletal animation controller state machine."},
    {"engine::runtime::CameraComponent", "Camera", "Gameplay",
     "Authored camera: pose follows this object's transform and publishes "
     "into the CameraManager priority/blend stack when active."},
};

} // namespace

const FieldMetadata *find_field_metadata(const char *typeName,
                                         const char *fieldName) noexcept {
  if ((typeName == nullptr) || (fieldName == nullptr)) {
    return nullptr;
  }
  for (const FieldMetadata &row : kFieldMetadataTable) {
    if ((std::strcmp(row.typeName, typeName) == 0) &&
        (std::strcmp(row.fieldName, fieldName) == 0)) {
      return &row;
    }
  }
  return nullptr;
}

const ComponentMetadata *find_component_metadata(const char *typeName) noexcept {
  if (typeName == nullptr) {
    return nullptr;
  }
  for (const ComponentMetadata &row : kComponentMetadataTable) {
    if (std::strcmp(row.typeName, typeName) == 0) {
      return &row;
    }
  }
  return nullptr;
}

math::Vec3 euler_degrees_from_quat(const math::Quat &rotation) noexcept {
  float pitch = 0.0F;
  float yaw = 0.0F;
  float roll = 0.0F;
  static_cast<void>(math::to_euler(rotation, &pitch, &yaw, &roll));
  return math::Vec3(pitch * kRadToDeg, yaw * kRadToDeg, roll * kRadToDeg);
}

math::Quat quat_from_euler_degrees(const math::Vec3 &degrees) noexcept {
  return math::from_euler(degrees.x * kDegToRad, degrees.y * kDegToRad,
                          degrees.z * kDegToRad);
}

const char *inspector_layer_name(std::uint32_t index) noexcept {
  // Fixed literal table (no lazy static init, no per-call formatting) until
  // issue #163 replaces these placeholder names with project-authored ones.
  static constexpr const char *kNames[kInspectorLayerCount] = {
      "Layer 0",  "Layer 1",  "Layer 2",  "Layer 3",  "Layer 4",  "Layer 5",
      "Layer 6",  "Layer 7",  "Layer 8",  "Layer 9",  "Layer 10", "Layer 11",
      "Layer 12", "Layer 13", "Layer 14", "Layer 15", "Layer 16", "Layer 17",
      "Layer 18", "Layer 19", "Layer 20", "Layer 21", "Layer 22", "Layer 23",
      "Layer 24", "Layer 25", "Layer 26", "Layer 27", "Layer 28", "Layer 29",
      "Layer 30", "Layer 31",
  };
  if (index >= kInspectorLayerCount) {
    return nullptr;
  }
  return kNames[index];
}

} // namespace engine::editor
