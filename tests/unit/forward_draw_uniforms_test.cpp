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
// The partition and the program resolution are here too because they are
// the other half of one contract: a pass binds a program once per run, so
// a run that is wrong, or a run id that resolves to the wrong program,
// shades a draw as something the material never asked for.

#include "command_buffer_flush_internal.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/core/logging.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"

#include "../fake_render_device.h"

namespace engine::renderer {

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

/// Counts the renderer's report of a program a material named but no
/// registration filled. A draw is per frame, so this is a report that has
/// to be latched: one per run, not one per draw.
void count_missing_program_reports(engine::core::LogLevel level,
                                   const char *channel, const char *message,
                                   void *userData) noexcept {
  if ((level != engine::core::LogLevel::Warning) || (channel == nullptr) ||
      (message == nullptr) || (userData == nullptr)) {
    return;
  }
  if (std::strcmp(channel, "renderer") != 0) {
    return;
  }
  if (std::strstr(message, "shading program") == nullptr) {
    return;
  }
  *static_cast<int *>(userData) += 1;
}

} // namespace

/// Runs this executable or test program.
/// Collects every program run of [start, end) through the cursor, writing
/// at most `capacity` and returning how many there were.
std::size_t collect_runs(const engine::renderer::CommandBufferView &view,
                         std::size_t start, std::size_t end,
                         engine::renderer::ShadingProgramRun *out,
                         std::size_t capacity) noexcept {
  std::size_t count = 0U;
  engine::renderer::ShadingProgramRun run{};
  for (std::size_t cursor = start;
       engine::renderer::next_program_run(view, &cursor, end, &run);) {
    if (count < capacity) {
      out[count] = run;
    }
    ++count;
  }
  return count;
}

int main() {
  // No device is live: the projection helpers fall back to the GL clip
  // conventions, and every table under test is passed in explicitly.
  engine::tests::fake_log().present = false;
  // Registered from the start because the report below is latched for the
  // process: a sink installed later would see nothing and the count would
  // read as "reported once" for the wrong reason.
  int missingProgramReports = 0;
  const bool loggingReady = engine::core::initialize_logging();
  const bool sinkReady =
      loggingReady && engine::core::log_register_sink(
                          &count_missing_program_reports,
                          &missingProgramReports);

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
        collect_runs(view, 0U, 6U, runs, kShadingModelCount);
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

    // A sub-range is walked on its own terms, which is what the deferred
    // path does for the transparent tail.
    const std::size_t tail =
        collect_runs(view, 2U, 6U, runs, kShadingModelCount);
    check((tail == 2U) && (runs[0].first == 2U) && (runs[0].count == 3U) &&
              (runs[1].first == 5U) && (runs[1].count == 1U),
          "a sub-range walks from its own start");

    // One model is one run, which is every scene that mixes none.
    DrawCommand uniform[4] = {keyed(0U), keyed(0U), keyed(0U), keyed(0U)};
    CommandBufferView uniformView{};
    uniformView.data = uniform;
    uniformView.count = 4U;
    const std::size_t single =
        collect_runs(uniformView, 0U, 4U, runs, kShadingModelCount);
    check((single == 1U) && (runs[0].count == 4U),
          "one model in a range is one run");

    // Boundaries: an empty range and null arguments answer without
    // reading past anything.
    check(collect_runs(view, 3U, 3U, runs, kShadingModelCount) == 0U,
          "an empty range has no runs");
    ShadingProgramRun unused{};
    std::size_t cursor = 0U;
    check(!next_program_run(view, nullptr, 6U, &unused) &&
              !next_program_run(view, &cursor, 6U, nullptr),
          "a null cursor or destination yields no run");

    // A range that runs past the view stops at the view.
    const std::size_t clamped =
        collect_runs(view, 0U, 99U, runs, kShadingModelCount);
    std::size_t clampedTotal = 0U;
    for (std::size_t i = 0U; i < clamped; ++i) {
      clampedTotal += runs[i].count;
    }
    check(clampedTotal == 6U, "a range past the view stops at the view");

    // A depth-sorted transparent range alternates programs as often as the
    // scene interleaves them -- far more times than there are programs.
    // Every run must carry its own draws' program: a fixed run table used
    // to join everything past its 128th run onto the last one, drawing it
    // with the wrong program.
    constexpr std::size_t kAlternating = 300U;
    static DrawCommand alternating[kAlternating] = {};
    for (std::size_t i = 0U; i < kAlternating; ++i) {
      alternating[i] = keyed(static_cast<std::uint8_t>(i % 2U));
    }
    CommandBufferView alternatingView{};
    alternatingView.data = alternating;
    alternatingView.count = static_cast<std::uint32_t>(kAlternating);
    std::size_t walked = 0U;
    bool everyRunOwnProgram = true;
    ShadingProgramRun run{};
    for (std::size_t at = 0U;
         next_program_run(alternatingView, &at, kAlternating, &run);) {
      everyRunOwnProgram =
          everyRunOwnProgram && (run.count == 1U) && (run.first == walked) &&
          (run.programId ==
           draw_key_shading_model(alternating[run.first].sortKey));
      ++walked;
    }
    check(walked == kAlternating,
          "300 alternating draws are 300 runs, not a capped table");
    check(everyRunOwnProgram, "every alternating run keeps its own program");
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
        collect_runs(fiveView, 0U, 5U, fiveRuns, kMaxShadingPrograms);
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
        collect_runs(sparseView, 0U, 4U, sparseRuns, kMaxShadingPrograms);
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
    const std::size_t fullCount = collect_runs(
        fullView, 0U, kMaxShadingPrograms, fullRuns, kMaxShadingPrograms);
    std::size_t fullCovered = 0U;
    for (std::size_t i = 0U; i < fullCount; ++i) {
      fullCovered += fullRuns[i].count;
    }
    check(fullCount == kMaxShadingPrograms,
          "every addressable program gets its own run at capacity");
    check(fullCovered == kMaxShadingPrograms,
          "the runs at capacity still cover every draw");

    // One run past the program count: the draw after the last program
    // returns to the first, as a depth-sorted transparent range does. It
    // is its own run with its own program; there is no run table to fill.
    DrawCommand pastFull[kMaxShadingPrograms + 1U] = {};
    for (std::size_t i = 0U; i < kMaxShadingPrograms; ++i) {
      pastFull[i] = programKeyed(static_cast<std::uint8_t>(i));
    }
    pastFull[kMaxShadingPrograms] = programKeyed(0U);
    CommandBufferView pastView{};
    pastView.data = pastFull;
    pastView.count = static_cast<std::uint32_t>(kMaxShadingPrograms + 1U);
    ShadingProgramRun lastRun{};
    std::size_t pastCount = 0U;
    for (std::size_t cursor = 0U; next_program_run(
             pastView, &cursor, kMaxShadingPrograms + 1U, &lastRun);) {
      ++pastCount;
    }
    check(pastCount == (kMaxShadingPrograms + 1U),
          "one run past the program count is still its own run");
    check((lastRun.programId == 0U) && (lastRun.first == kMaxShadingPrograms) &&
              (lastRun.count == 1U),
          "the run past the program count draws with its own program");
  }

  // What a run's program id resolves to. Addressable is not registered:
  // the table is as wide as the key's field so that no id can index past
  // it, which means most ids read an empty slot in a normal build and the
  // fallback is the common path rather than the exceptional one. A
  // material naming a program that did not load has to draw as physically
  // based; drawing with an empty handle or refusing the frame are both
  // worse than a wrong-looking surface.
  {
    BackendState resolve{};
    resolve.pbrProgram = DeviceProgramHandle{11};
    resolve.shadingPrograms[shading_program_id(ShadingModel::Pbr)] =
        resolve.pbrProgram;
    resolve.shadingPrograms[shading_program_id(ShadingModel::Toon)] =
        DeviceProgramHandle{22};

    check(shading_program(resolve, shading_program_id(ShadingModel::Toon)) ==
              DeviceProgramHandle{22},
          "a registered program id resolves to its own program");
    check(shading_program(resolve, shading_program_id(ShadingModel::Unlit)) ==
              resolve.pbrProgram,
          "a program that did not load falls back to physically based");
    check(shading_program(resolve, static_cast<std::uint8_t>(
                                       kMaxShadingPrograms - 1U)) ==
              resolve.pbrProgram,
          "the last addressable id falls back rather than reading past the "
          "table");

    // The fallback is not a guarantee of validity. A build where the PBR
    // program itself failed to load has already refused to start, so this
    // pins that the resolution reports what it has rather than inventing
    // a handle.
    BackendState empty{};
    check(shading_program(empty, shading_program_id(ShadingModel::Toon)) ==
              kInvalidDeviceProgram,
          "with nothing registered the resolution reports no program");

    // Three unregistered resolutions above, one report. The resolution runs
    // once per run per pass per frame, so a report per occurrence would bury
    // the log within a second of starting and the diagnostic that says a
    // material is drawing as the wrong thing would stop being readable.
    if (sinkReady) {
      check(missingProgramReports == 1,
            "an unregistered program is reported once for the run, not once "
            "per resolution");
    } else {
      std::fprintf(stderr, "SKIPPED: no log sink, so the report's latching "
                           "is unchecked\n");
    }
  }

  if (sinkReady) {
    engine::core::log_unregister_sink(&count_missing_program_reports,
                                      &missingProgramReports);
  }
  if (loggingReady) {
    engine::core::shutdown_logging();
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
