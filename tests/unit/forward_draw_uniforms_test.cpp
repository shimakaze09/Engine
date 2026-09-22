// Pins the per-draw forward uniform set against a recording device, and
// the shading-model run partition the passes bind programs by. The
// forward pass, the deferred path's transparent pass and every scene
// capture each used to carry their own copy of this upload, so a uniform
// added to one draw could be forgotten in the other two and the same
// material would shade differently depending on which pass drew it. They
// now share one helper, and this suite asserts what that helper writes:
// every location the program declares, the albedo fallback rather than a
// dangling render target, and the instancing toggle cleared per draw.
//
// The partition is here too because it is the other half of one
// contract: a pass binds a program once per run, so a run that is wrong
// shades a draw with the wrong model.

#include "command_buffer_flush_internal.h"

#include <cstdio>
#include <vector>

#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

// The pass-level helpers this suite does not exercise. The shared
// forward-draw helper calls the foliage upload, which is per draw, so
// that one records through the device like any other.
const RenderDevice *render_device() noexcept { return nullptr; }

/// No texture store in this suite, so every material's albedo slot reads
/// as unset and the fallback path is what each draw exercises.
DeviceTextureHandle texture_device_handle(TextureHandle handle) noexcept {
  static_cast<void>(handle);
  return kInvalidDeviceTexture;
}

} // namespace engine::renderer

namespace {

using namespace engine::renderer;

/// One recorded device call: which entry point, and which parameter.
struct Call final {
  const char *entry = "";
  std::int32_t param = -1;
};

std::vector<Call> g_calls;
std::vector<std::uint32_t> g_textureSlots;
int g_drawIndexed = 0;
int g_draw = 0;

void record(const char *entry, ShaderParam param) noexcept {
  g_calls.push_back(Call{entry, param.value});
}

void fake_set_param_mat4(ShaderParam p, const float *) noexcept {
  record("mat4", p);
}
void fake_set_param_mat3(ShaderParam p, const float *) noexcept {
  record("mat3", p);
}
void fake_set_param_f32(ShaderParam p, float) noexcept { record("f32", p); }
void fake_set_param_i32(ShaderParam p, std::int32_t) noexcept {
  record("i32", p);
}
void fake_set_param_vec2(ShaderParam p, const float *) noexcept {
  record("vec2", p);
}
void fake_set_param_vec3(ShaderParam p, const float *) noexcept {
  record("vec3", p);
}
void fake_set_param_vec4(ShaderParam p, const float *) noexcept {
  record("vec4", p);
}
void fake_bind_texture_slot(std::uint32_t slot,
                            DeviceTextureHandle) noexcept {
  g_textureSlots.push_back(slot);
}
void fake_draw_indexed(DeviceGeometryHandle, std::int32_t) noexcept {
  ++g_drawIndexed;
}
void fake_draw(DeviceGeometryHandle, PrimitiveTopology, std::int32_t,
               std::int32_t) noexcept {
  ++g_draw;
}

RenderDevice make_recording_device() noexcept {
  RenderDevice device{};
  device.set_param_mat4 = &fake_set_param_mat4;
  device.set_param_mat3 = &fake_set_param_mat3;
  device.set_param_f32 = &fake_set_param_f32;
  device.set_param_i32 = &fake_set_param_i32;
  device.set_param_vec2 = &fake_set_param_vec2;
  device.set_param_vec3 = &fake_set_param_vec3;
  device.set_param_vec4 = &fake_set_param_vec4;
  device.bind_texture_slot = &fake_bind_texture_slot;
  device.draw_indexed = &fake_draw_indexed;
  device.draw = &fake_draw;
  return device;
}

/// Distinct, valid locations so a missing upload shows up as a missing
/// parameter rather than as a coincidence of two fields sharing one slot.
ForwardDrawProgram make_program() noexcept {
  ForwardDrawProgram program{};
  std::int32_t next = 1;
  const auto take = [&next]() noexcept {
    ShaderParam param{};
    param.value = next;
    ++next;
    return param;
  };
  program.albedo = take();
  program.roughness = take();
  program.metallic = take();
  program.opacity = take();
  program.emissive = take();
  program.hasAlbedoTexture = take();
  program.model = take();
  program.mvp = take();
  program.normalMatrix = take();
  program.useInstancing = take();
  program.materialTextures.hasMetallicRoughness = take();
  program.materialTextures.metallicRoughnessMap = take();
  program.materialTextures.hasEmissive = take();
  program.materialTextures.emissiveMap = take();
  program.materialTextures.hasOcclusion = take();
  program.materialTextures.occlusionMap = take();
  program.materialTextures.hasOpacity = take();
  program.materialTextures.opacityMap = take();
  program.materialTextures.alphaMode = take();
  program.materialTextures.alphaCutoff = take();
  program.materialTextures.uvTiling = take();
  program.materialTextures.uvOffset = take();
  return program;
}

int g_failures = 0;
void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

/// True when some recorded call used `param`.
bool wrote(std::int32_t param) noexcept {
  for (const Call &call : g_calls) {
    if (call.param == param) {
      return true;
    }
  }
  return false;
}

/// How many recorded calls used `param`.
int write_count(std::int32_t param) noexcept {
  int count = 0;
  for (const Call &call : g_calls) {
    if (call.param == param) {
      ++count;
    }
  }
  return count;
}

void reset() noexcept {
  g_calls.clear();
  g_textureSlots.clear();
  g_drawIndexed = 0;
  g_draw = 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  const RenderDevice device = make_recording_device();
  const ForwardDrawProgram program = make_program();

  BackendState backend{};
  backend.fallbackTexture2D = DeviceTextureHandle{7};

  GpuMesh mesh{};
  mesh.geometry = DeviceGeometryHandle{1};
  mesh.vertexCount = 3U;
  mesh.indexCount = 3U;

  DrawCommand command{};
  command.mesh = MeshHandle{1};
  command.material.roughness = 0.25F;
  command.material.metallic = 0.5F;
  command.material.opacity = 1.0F;

  // Every location the program declares is written by one draw. This is
  // the set the three passes have to agree on, and the reason it is
  // asserted by location rather than by name: a field the helper forgets
  // leaves its parameter untouched.
  {
    reset();
    ForwardDrawBindings bindings{};
    upload_forward_material(program, backend, &device, command, &bindings);
    draw_forward_command(program, &device, command, mesh,
                         engine::math::Mat4(), nullptr);
    check(g_drawIndexed == 0,
          "a null stats pointer draws nothing rather than crashing");

    RendererFrameStats stats{};
    reset();
    ForwardDrawBindings drawBindings{};
    upload_forward_material(program, backend, &device, command,
                            &drawBindings);
    draw_forward_command(program, &device, command, mesh,
                         engine::math::Mat4(), &stats);

    check(wrote(program.albedo.value), "the draw writes albedo");
    check(wrote(program.roughness.value), "the draw writes roughness");
    check(wrote(program.metallic.value), "the draw writes metallic");
    check(wrote(program.opacity.value), "the draw writes opacity");
    check(wrote(program.emissive.value), "the draw writes emissive");
    check(wrote(program.hasAlbedoTexture.value),
          "the draw says whether it has an albedo texture");
    check(wrote(program.model.value), "the draw writes the model matrix");
    check(wrote(program.mvp.value), "the draw writes the mvp");
    check(wrote(program.normalMatrix.value),
          "the draw writes the normal matrix");
    check(wrote(program.materialTextures.alphaMode.value),
          "the draw writes the alpha mode");
    check(wrote(program.materialTextures.alphaCutoff.value),
          "the draw writes the alpha cutoff");
    check(wrote(program.materialTextures.uvTiling.value),
          "the draw writes the uv tiling");
    check(wrote(program.materialTextures.uvOffset.value),
          "the draw writes the uv offset");
    check(wrote(program.materialTextures.hasMetallicRoughness.value) &&
              wrote(program.materialTextures.hasEmissive.value) &&
              wrote(program.materialTextures.hasOcclusion.value) &&
              wrote(program.materialTextures.hasOpacity.value),
          "the draw says which of the four texture slots it has");

    // A material with no albedo texture binds the opaque fallback, not
    // nothing: a declared sampler left pointing at the pass's own render
    // target is a draw WebGL rejects.
    check(!g_textureSlots.empty() && (g_textureSlots[0] == 0U),
          "a textureless material still binds slot 0");

    // Cleared per draw, because the opaque batching path sets it to 1 to
    // issue a batch and a later single draw would read its transform
    // from the instance buffer.
    check(wrote(program.useInstancing.value),
          "the draw clears the instancing toggle");

    check(stats.drawCalls == 1U, "one draw counts one draw call");
    check(stats.triangleCount == 1U, "a three-index mesh counts one triangle");
    check(g_drawIndexed == 1, "an indexed mesh draws indexed");
  }

  // A second draw of the same material does not rebind the textures it
  // already has bound, which is the only reason the binding state is a
  // parameter rather than a local.
  {
    RendererFrameStats stats{};
    reset();
    ForwardDrawBindings bindings{};
    upload_forward_material(program, backend, &device, command, &bindings);
    const std::size_t afterFirst = g_textureSlots.size();
    upload_forward_material(program, backend, &device, command, &bindings);
    check(g_textureSlots.size() == afterFirst,
          "a repeated material rebinds no texture");
    static_cast<void>(stats);
  }

  // A location the program does not declare is skipped rather than
  // written to an invalid slot: a program without the instancing toggle
  // is the bgfx case, and a draw must not write parameter -1.
  {
    RendererFrameStats stats{};
    ForwardDrawProgram partial = program;
    partial.useInstancing = kInvalidShaderParam;
    partial.model = kInvalidShaderParam;
    reset();
    ForwardDrawBindings bindings{};
    upload_forward_material(partial, backend, &device, command, &bindings);
    draw_forward_command(partial, &device, command, mesh,
                         engine::math::Mat4(), &stats);
    check(write_count(kInvalidShaderParam.value) == 0,
          "an undeclared location is never written");
    check(wrote(program.mvp.value),
          "the declared locations are still written");
  }

  // A mesh with no index buffer draws non-indexed and counts its own
  // triangles.
  {
    RendererFrameStats stats{};
    GpuMesh unindexed = mesh;
    unindexed.indexCount = 0U;
    unindexed.vertexCount = 6U;
    reset();
    draw_forward_command(program, &device, command, unindexed,
                         engine::math::Mat4(), &stats);
    check((g_drawIndexed == 0) && (g_draw == 1) && (stats.drawCalls == 1U) &&
              (stats.triangleCount == 2U),
          "an unindexed mesh draws non-indexed and counts two triangles");
  }

  // The run partition. Render prep sorts the shading model directly
  // below the transparency bit, so each model occupies one contiguous
  // run and a pass binds its program once per run.
  {
    DrawCommand commands[6] = {};
    const auto keyed = [](std::uint8_t model) noexcept {
      DrawCommand entry{};
      entry.sortKey.value =
          draw_key_shading_model_bits(static_cast<ShadingModel>(model));
      entry.material.shadingModel = static_cast<ShadingModel>(model);
      return entry;
    };
    commands[0] = keyed(0U);
    commands[1] = keyed(0U);
    commands[2] = keyed(1U);
    commands[3] = keyed(1U);
    commands[4] = keyed(1U);
    commands[5] = keyed(2U);
    CommandBufferView view{};
    view.data = commands;
    view.count = 6U;

    ShadingProgramRun runs[kShadingModelCount] = {};
    const std::size_t count =
        partition_program_runs(view, 0U, 6U, runs, kShadingModelCount);
    check(count == 3U, "three models give three runs");
    check((runs[0].programId == 0U) && (runs[0].first == 0U) &&
              (runs[0].count == 2U),
          "the first run covers the first model's draws");
    check((runs[1].programId == 1U) && (runs[1].first == 2U) &&
              (runs[1].count == 3U),
          "the second run covers the second model's draws");
    check((runs[2].programId == 2U) && (runs[2].first == 5U) &&
              (runs[2].count == 1U),
          "the third run covers the last draw");

    // Every draw lands in exactly one run: a gap would silently drop a
    // draw, an overlap would draw one twice.
    std::size_t covered = 0U;
    for (std::size_t i = 0U; i < count; ++i) {
      check(runs[i].first == covered, "runs are contiguous from the start");
      covered += runs[i].count;
    }
    check(covered == 6U, "the runs cover every draw exactly once");

    // A sub-range is partitioned on its own terms, which is what the
    // deferred path does for the transparent tail.
    const std::size_t tail =
        partition_program_runs(view, 2U, 6U, runs, kShadingModelCount);
    check((tail == 2U) && (runs[0].first == 2U) && (runs[0].count == 3U) &&
              (runs[1].first == 5U) && (runs[1].count == 1U),
          "a sub-range partitions from its own start");

    // One model is one run, which is every scene that mixes none.
    DrawCommand uniform[4] = {keyed(0U), keyed(0U), keyed(0U), keyed(0U)};
    CommandBufferView uniformView{};
    uniformView.data = uniform;
    uniformView.count = 4U;
    const std::size_t single = partition_program_runs(
        uniformView, 0U, 4U, runs, kShadingModelCount);
    check((single == 1U) && (runs[0].count == 4U),
          "one model in a range is one run");

    // Boundaries: an empty range, a null destination, and a capacity of
    // one all answer without reading past anything.
    check(partition_program_runs(view, 3U, 3U, runs,
                                       kShadingModelCount) == 0U,
          "an empty range has no runs");
    check(partition_program_runs(view, 0U, 6U, nullptr,
                                       kShadingModelCount) == 0U,
          "a null destination yields no runs");
    const std::size_t capped =
        partition_program_runs(view, 0U, 6U, runs, 1U);
    check((capped == 1U) && (runs[0].count == 6U),
          "a capacity of one joins the tail onto the run it can hold, "
          "rather than dropping those draws");

    // A range that runs past the view stops at the view.
    const std::size_t clamped =
        partition_program_runs(view, 0U, 99U, runs, kShadingModelCount);
    std::size_t clampedTotal = 0U;
    for (std::size_t i = 0U; i < clamped; ++i) {
      clampedTotal += runs[i].count;
    }
    check(clampedTotal == 6U, "a range past the view stops at the view");
  }

  // Program ids beyond the three the engine ships. The partition reads the
  // key's field and nothing else, so it must not have been sized or
  // reasoned around exactly three: with three, one run per program is
  // indistinguishable from a coincidence.
  {
    const auto programKeyed = [](std::uint8_t programId) noexcept {
      DrawCommand entry{};
      entry.sortKey.value =
          (static_cast<std::uint64_t>(programId) & kDrawKeyShadingModelMask)
          << kDrawKeyShadingModelShift;
      return entry;
    };

    // The key's field is seven bits, so the largest id it can carry is
    // one below the addressable limit. Nothing a real key holds can be
    // unaddressable, which is what makes the bound in shading_program a
    // guard rather than a live path.
    DrawKey saturated{};
    saturated.value = kDrawKeyShadingModelMask << kDrawKeyShadingModelShift;
    check(draw_key_shading_model(saturated) ==
              static_cast<std::uint8_t>(kMaxShadingPrograms - 1U),
          "the key's widest program field is the last addressable id");
    check(shading_program_id_is_addressable(
              draw_key_shading_model(saturated)),
          "every id a key can carry is addressable");

    // Five programs, including ids no shipped model occupies.
    DrawCommand five[5] = {programKeyed(0U), programKeyed(1U),
                           programKeyed(2U), programKeyed(3U),
                           programKeyed(4U)};
    CommandBufferView fiveView{};
    fiveView.data = five;
    fiveView.count = 5U;
    ShadingProgramRun fiveRuns[kMaxShadingPrograms] = {};
    const std::size_t fiveCount =
        partition_program_runs(fiveView, 0U, 5U, fiveRuns,
                               kMaxShadingPrograms);
    check(fiveCount == 5U, "five programs give five runs");
    bool fiveNamed = true;
    for (std::size_t i = 0U; i < fiveCount; ++i) {
      fiveNamed = fiveNamed && (fiveRuns[i].programId ==
                                static_cast<std::uint8_t>(i)) &&
                  (fiveRuns[i].first == i) && (fiveRuns[i].count == 1U);
    }
    check(fiveNamed, "each of the five runs names its own program");

    // Ids that are neither contiguous nor small, so no arithmetic that
    // assumes id equals run index survives.
    DrawCommand sparse[4] = {programKeyed(3U), programKeyed(40U),
                             programKeyed(40U), programKeyed(127U)};
    CommandBufferView sparseView{};
    sparseView.data = sparse;
    sparseView.count = 4U;
    ShadingProgramRun sparseRuns[kMaxShadingPrograms] = {};
    const std::size_t sparseCount =
        partition_program_runs(sparseView, 0U, 4U, sparseRuns,
                               kMaxShadingPrograms);
    check((sparseCount == 3U) && (sparseRuns[0].programId == 3U) &&
              (sparseRuns[1].programId == 40U) &&
              (sparseRuns[1].count == 2U) &&
              (sparseRuns[2].programId == 127U),
          "sparse high program ids each get their own run");

    // Exactly at capacity: one draw per addressable program.
    DrawCommand full[kMaxShadingPrograms] = {};
    for (std::size_t i = 0U; i < kMaxShadingPrograms; ++i) {
      full[i] = programKeyed(static_cast<std::uint8_t>(i));
    }
    CommandBufferView fullView{};
    fullView.data = full;
    fullView.count = static_cast<std::uint32_t>(kMaxShadingPrograms);
    ShadingProgramRun fullRuns[kMaxShadingPrograms] = {};
    const std::size_t fullCount = partition_program_runs(
        fullView, 0U, kMaxShadingPrograms, fullRuns, kMaxShadingPrograms);
    std::size_t fullCovered = 0U;
    for (std::size_t i = 0U; i < fullCount; ++i) {
      fullCovered += fullRuns[i].count;
    }
    check(fullCount == kMaxShadingPrograms,
          "every addressable program gets its own run at capacity");
    check(fullCovered == kMaxShadingPrograms,
          "the runs at capacity still cover every draw");

    // One run past what the caller can hold: the tail joins the last run
    // rather than being dropped, and every draw is still covered.
    ShadingProgramRun tightRuns[kMaxShadingPrograms - 1U] = {};
    const std::size_t tightCount =
        partition_program_runs(fullView, 0U, kMaxShadingPrograms, tightRuns,
                               kMaxShadingPrograms - 1U);
    std::size_t tightCovered = 0U;
    for (std::size_t i = 0U; i < tightCount; ++i) {
      tightCovered += tightRuns[i].count;
    }
    check(tightCount == (kMaxShadingPrograms - 1U),
          "one run past capacity returns exactly capacity");
    check(tightCovered == kMaxShadingPrograms,
          "one run past capacity still draws every draw");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "forward_draw_uniforms_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("forward_draw_uniforms_test: the per-draw uniform set is "
              "pinned\n");
  return 0;
}
