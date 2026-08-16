// Verifies the Inspector's semantic metadata tables and the pure Euler-
// degrees<->quaternion round-trip helpers (issue #156): field/component
// metadata lookups hit for annotated rows and miss safely for unknown
// ones, and the Euler round trip is exact away from the pitch = +-90 deg
// gimbal pole and stays a valid (renormalized) rotation at the pole.

#include "editor_inspector_metadata.h"

#include "engine/math/quat.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

namespace math = engine::math;

using engine::editor::ComponentMetadata;
using engine::editor::FieldMetadata;
using engine::editor::InspectorWidget;

constexpr float kPi = 3.14159265358979323846F;

bool nearly_equal(float a, float b, float tolerance) noexcept {
  return std::fabs(a - b) <= tolerance;
}

/// A known annotated field (Transform.rotation) resolves to the expected
/// widget kind and units; an unannotated/unknown lookup misses cleanly.
int check_field_metadata_lookup() noexcept {
  const FieldMetadata *rotation = engine::editor::find_field_metadata(
      "engine::runtime::Transform", "rotation");
  if (rotation == nullptr) {
    return 1;
  }
  if (rotation->widget != InspectorWidget::EulerDegrees) {
    return 2;
  }
  if ((rotation->units == nullptr) || (std::strcmp(rotation->units, "deg") != 0)) {
    return 3;
  }

  const FieldMetadata *layer = engine::editor::find_field_metadata(
      "engine::runtime::Collider", "collisionLayer");
  if ((layer == nullptr) || (layer->widget != InspectorWidget::LayerMask)) {
    return 4;
  }

  const FieldMetadata *shape = engine::editor::find_field_metadata(
      "engine::runtime::Collider", "shape");
  if ((shape == nullptr) || (shape->widget != InspectorWidget::Enum) ||
      (shape->enumLabels == nullptr) || (shape->enumLabelCount != 3U) ||
      (std::strcmp(shape->enumLabels[0], "Box") != 0) ||
      (std::strcmp(shape->enumLabels[1], "Sphere") != 0) ||
      (std::strcmp(shape->enumLabels[2], "Capsule") != 0)) {
    return 8;
  }

  const FieldMetadata *lightType = engine::editor::find_field_metadata(
      "engine::runtime::LightComponent", "type");
  if ((lightType == nullptr) || (lightType->widget != InspectorWidget::Enum) ||
      (lightType->enumLabelCount != 2U)) {
    return 9;
  }

  if (engine::editor::find_field_metadata(nullptr, "rotation") != nullptr) {
    return 5;
  }
  if (engine::editor::find_field_metadata("engine::runtime::Transform",
                                          "doesNotExist") != nullptr) {
    return 6;
  }
  if (engine::editor::find_field_metadata("engine::runtime::NotAType",
                                          "position") != nullptr) {
    return 7;
  }
  return 0;
}

/// Every persistent component type has a display name distinct from a
/// missing-lookup nullptr, and an unknown type name misses cleanly.
int check_component_metadata_lookup() noexcept {
  const ComponentMetadata *transform = engine::editor::find_component_metadata(
      "engine::runtime::Transform");
  if ((transform == nullptr) || (transform->displayName == nullptr)) {
    return 1;
  }
  if (std::strcmp(transform->displayName, "Transform") != 0) {
    return 2;
  }

  const ComponentMetadata *mesh = engine::editor::find_component_metadata(
      "engine::runtime::MeshComponent");
  if ((mesh == nullptr) || (std::strcmp(mesh->category, "Rendering") != 0)) {
    return 3;
  }

  if (engine::editor::find_component_metadata("engine::runtime::NotAType") !=
      nullptr) {
    return 4;
  }
  if (engine::editor::find_component_metadata(nullptr) != nullptr) {
    return 5;
  }
  return 0;
}

/// Round trip through degrees is exact (within float tolerance) for angle
/// triples away from the pitch = +-90 deg gimbal pole -- the documented
/// exact case in euler_degrees_from_quat's declaration comment.
int check_euler_round_trip_away_from_pole() noexcept {
  const float kCases[][3] = {
      {0.0F, 0.0F, 0.0F},
      {30.0F, 45.0F, 60.0F},
      {-20.0F, 70.0F, -135.0F},
      {10.0F, -10.0F, 10.0F},
      {89.0F, 15.0F, -15.0F},
      {-89.0F, -15.0F, 15.0F},
  };
  for (const auto &c : kCases) {
    const math::Vec3 degreesIn(c[0], c[1], c[2]);
    const math::Quat q = engine::editor::quat_from_euler_degrees(degreesIn);
    const math::Vec3 degreesOut = engine::editor::euler_degrees_from_quat(q);
    const math::Quat roundTrip =
        engine::editor::quat_from_euler_degrees(degreesOut);

    // Compare the reconstructed quaternion, not the raw angles, since a
    // sign-flipped quaternion (q and -q) represents the identical rotation;
    // |dot| ~= 1 is the correct equivalence test.
    const float dot = (q.x * roundTrip.x) + (q.y * roundTrip.y) +
                      (q.z * roundTrip.z) + (q.w * roundTrip.w);
    if (!nearly_equal(std::fabs(dot), 1.0F, 1.0e-4F)) {
      return 1;
    }
  }
  return 0;
}

/// At the pitch = +-90 deg gimbal pole, the round trip still reproduces the
/// same rotation (documented as the one case where the recovered
/// yaw/roll split may differ from the original input split).
int check_euler_round_trip_at_pole_preserves_rotation() noexcept {
  const math::Vec3 degreesIn(90.0F, 40.0F, 20.0F);
  const math::Quat q = engine::editor::quat_from_euler_degrees(degreesIn);
  const math::Vec3 degreesOut = engine::editor::euler_degrees_from_quat(q);
  const math::Quat roundTrip =
      engine::editor::quat_from_euler_degrees(degreesOut);

  const float dot = (q.x * roundTrip.x) + (q.y * roundTrip.y) +
                    (q.z * roundTrip.z) + (q.w * roundTrip.w);
  if (!nearly_equal(std::fabs(dot), 1.0F, 1.0e-4F)) {
    return 1;
  }

  // The result stays a unit quaternion (no drift/denormalization through
  // the degrees round trip).
  const float lengthSq = (roundTrip.x * roundTrip.x) +
                        (roundTrip.y * roundTrip.y) +
                        (roundTrip.z * roundTrip.z) +
                        (roundTrip.w * roundTrip.w);
  if (!nearly_equal(lengthSq, 1.0F, 1.0e-4F)) {
    return 2;
  }
  return 0;
}

/// Identity quaternion maps to zero degrees on every axis.
int check_euler_identity() noexcept {
  const math::Vec3 degrees =
      engine::editor::euler_degrees_from_quat(math::Quat());
  if (!nearly_equal(degrees.x, 0.0F, 1.0e-5F) ||
      !nearly_equal(degrees.y, 0.0F, 1.0e-5F) ||
      !nearly_equal(degrees.z, 0.0F, 1.0e-5F)) {
    return 1;
  }
  return 0;
}

/// A pure 90 deg yaw-only rotation round-trips exactly through the widget
/// helpers (a common authoring case: turning an object to face a
/// direction).
int check_euler_yaw_only() noexcept {
  const math::Vec3 degreesIn(0.0F, 90.0F, 0.0F);
  const math::Quat q = engine::editor::quat_from_euler_degrees(degreesIn);
  const math::Vec3 degreesOut = engine::editor::euler_degrees_from_quat(q);
  if (!nearly_equal(degreesOut.x, 0.0F, 0.1F) ||
      !nearly_equal(degreesOut.y, 90.0F, 0.1F) ||
      !nearly_equal(degreesOut.z, 0.0F, 0.1F)) {
    return 1;
  }
  return 0;
}

/// Layer names cover the full advertised slot count and reject out-of-
/// range indices instead of reading past the table.
int check_layer_names_bounded() noexcept {
  for (std::uint32_t i = 0U; i < engine::editor::kInspectorLayerCount; ++i) {
    if (engine::editor::inspector_layer_name(i) == nullptr) {
      return 1;
    }
  }
  if (engine::editor::inspector_layer_name(
          static_cast<std::uint32_t>(engine::editor::kInspectorLayerCount)) !=
      nullptr) {
    return 2;
  }
  return 0;
}

} // namespace

int main() {
  struct Case {
    const char *name;
    int (*fn)() noexcept;
  };
  const Case cases[] = {
      {"field_metadata_lookup", check_field_metadata_lookup},
      {"component_metadata_lookup", check_component_metadata_lookup},
      {"euler_round_trip_away_from_pole",
       check_euler_round_trip_away_from_pole},
      {"euler_round_trip_at_pole_preserves_rotation",
       check_euler_round_trip_at_pole_preserves_rotation},
      {"euler_identity", check_euler_identity},
      {"euler_yaw_only", check_euler_yaw_only},
      {"layer_names_bounded", check_layer_names_bounded},
  };
  for (const Case &c : cases) {
    const int result = c.fn();
    if (result != 0) {
      std::fprintf(stderr, "editor_inspector_metadata_test: %s failed: %d\n",
                   c.name, result);
      return result;
    }
  }
  std::printf("editor_inspector_metadata_test: all tests passed\n");
  return 0;
}
