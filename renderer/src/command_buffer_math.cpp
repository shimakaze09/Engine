// Implements private command buffer math and cache-key helpers.

#include "command_buffer_math.h"

#include <cstring>

namespace engine::renderer {
namespace {

constexpr std::uint64_t kFnv1a64Offset = 14695981039346656037ULL;
constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;

/// Appends one integer value to an FNV-1a hash.
std::uint64_t hash_u64(std::uint64_t hash, std::uint64_t value) noexcept {
  hash ^= value;
  return hash * kFnv1a64Prime;
}

/// Appends one finite float value to an FNV-1a hash.
std::uint64_t hash_float(std::uint64_t hash, float value) noexcept {
  std::uint32_t bits = 0U;
  if (value != 0.0F) {
    std::memcpy(&bits, &value, sizeof(bits));
  }
  return hash_u64(hash, bits);
}

/// Appends one vector value to an FNV-1a hash.
std::uint64_t hash_vec3(std::uint64_t hash, const math::Vec3 &value) noexcept {
  hash = hash_float(hash, value.x);
  hash = hash_float(hash, value.y);
  return hash_float(hash, value.z);
}

/// Appends one matrix value to an FNV-1a hash.
std::uint64_t hash_mat4(std::uint64_t hash, const math::Mat4 &value) noexcept {
  for (const math::Vec4 &column : value.columns) {
    hash = hash_float(hash, column.x);
    hash = hash_float(hash, column.y);
    hash = hash_float(hash, column.z);
    hash = hash_float(hash, column.w);
  }
  return hash;
}

} // namespace

math::Mat4 compute_model_matrix(const DrawCommand &command) noexcept {
  return command.modelMatrix;
}

math::Mat4 compute_mvp(const math::Mat4 &model,
                       const math::Mat4 &viewProjection) noexcept {
  return math::mul(viewProjection, model);
}

std::uint64_t directional_shadow_cache_key(
    CommandBufferView commandBufferView, std::size_t opaqueCount,
    const DirectionalLightData &light, const CascadeSplits &splits,
    const std::array<math::Mat4, kShadowCascadeCount> &matrices) noexcept {
  std::uint64_t hash = kFnv1a64Offset;
  hash = hash_u64(hash, static_cast<std::uint64_t>(opaqueCount));
  hash = hash_vec3(hash, light.direction);
  hash = hash_vec3(hash, light.color);
  hash = hash_float(hash, light.intensity);

  for (std::size_t i = 0U; i <= kShadowCascadeCount; ++i) {
    hash = hash_float(hash, splits.distances[i]);
  }
  for (const math::Mat4 &matrix : matrices) {
    hash = hash_mat4(hash, matrix);
  }

  for (std::size_t i = 0U; i < opaqueCount; ++i) {
    const DrawCommand &command = commandBufferView.data[i];
    hash = hash_u64(hash, command.sortKey.value);
    hash = hash_u64(hash, command.entity);
    hash = hash_u64(hash, command.mesh.id);
    hash = hash_float(hash, command.foliageWindStrength);
    hash = hash_float(hash, command.foliageWindFrequency);
    hash = hash_float(hash, command.foliageWindPhase);
    hash = hash_u64(hash, command.foliageLodIndex);
    hash = hash_mat4(hash, command.modelMatrix);
  }

  return hash;
}

void extract_normal_matrix(const math::Mat4 &model,
                           float *normalMatrixOut) noexcept {
  if (normalMatrixOut == nullptr) {
    return;
  }

  math::Mat4 invModel{};
  const math::Mat4 normalSource =
      math::inverse(model, &invModel) ? math::transpose(invModel) : model;

  normalMatrixOut[0] = normalSource.columns[0].x;
  normalMatrixOut[1] = normalSource.columns[0].y;
  normalMatrixOut[2] = normalSource.columns[0].z;

  normalMatrixOut[3] = normalSource.columns[1].x;
  normalMatrixOut[4] = normalSource.columns[1].y;
  normalMatrixOut[5] = normalSource.columns[1].z;

  normalMatrixOut[6] = normalSource.columns[2].x;
  normalMatrixOut[7] = normalSource.columns[2].y;
  normalMatrixOut[8] = normalSource.columns[2].z;
}

} // namespace engine::renderer
