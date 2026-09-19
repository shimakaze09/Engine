// GPU check for issue #565: the split-sum BRDF lookup the renderer bakes
// holds the right numbers at grazing angles, where the integrand is at its
// worst — 1 / NdotV grows without bound and the importance samples crowd
// the horizon. A CPU suite can run the same arithmetic, but not the cooked
// shader, its float precision, the RG16F target or the bake's draw; only a
// readback of the baked texture shows what a material will sample.
//
// The texture is copied texel for texel onto the back buffer with the
// engine's own present program and read back. Down the columns nearest
// NdotV = 0.02, 0.05 and 0.1, across every roughness row:
//
//   * scale and bias match a double-precision evaluation of the shader's
//     512-sample sum to one 8-bit level: the bake ran, and ran the sum;
//   * they are within 0.01 of the converged integral, which is what holds
//     the sum itself — its sample count and how its samples are laid out.
//     With the sample set's azimuth origin a quarter turn from the view
//     direction the same 512 samples left scale 0.04 out at NdotV = 0.02;
//   * bias never rises with roughness, and scale + bias never exceeds one.
//
// Scale alone is not monotone in roughness at these angles — the converged
// integral dips, climbs and falls again — so nothing is asserted of its
// direction. The readback is 8-bit UNORM and cannot carry a NaN or a value
// above one; agreement with a finite reference that stays below 0.9 at
// every texel is what rules both out.

#include "../gpu_scene_fixture.h"

#include "command_buffer_context.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

using engine::tests::CapturedFrame;

struct SplitSum final {
  double scale = 0.0;
  double bias = 0.0;
};

double radical_inverse(std::uint32_t bits) noexcept {
  bits = (bits << 16U) | (bits >> 16U);
  bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
  bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
  bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
  bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
  return static_cast<double>(bits) * 2.3283064365386963e-10;
}

double schlick_ggx(double nDot, double k) noexcept {
  return nDot / (nDot * (1.0 - k) + k);
}

/// The sum assets/shaders/bgfx/brdf_lut.fs.sc evaluates, term for term, in
/// double precision and with a chosen sample count. The normal is +z and
/// the view lies in the xz plane, so the tangent frame is the identity.
SplitSum integrate_split_sum(double nDotV, double roughness,
                             std::uint32_t sampleCount) noexcept {
  constexpr double kPi = 3.14159265358979323846;
  nDotV = (nDotV < 0.001) ? 0.001 : nDotV;
  roughness = (roughness < 0.001) ? 0.001 : roughness;
  const double a = roughness * roughness;
  const double k = a * 0.5;
  const double viewX = std::sqrt((1.0 - nDotV * nDotV > 0.0)
                                     ? (1.0 - nDotV * nDotV)
                                     : 0.0);
  SplitSum sum{};
  for (std::uint32_t i = 0U; i < sampleCount; ++i) {
    const double xiX = static_cast<double>(i) / static_cast<double>(sampleCount);
    const double xiY = radical_inverse(i);
    const double phi = 2.0 * kPi * xiX;
    const double cosTheta =
        std::sqrt((1.0 - xiY) / (1.0 + (a * a - 1.0) * xiY));
    const double sinSq = 1.0 - cosTheta * cosTheta;
    const double sinTheta = std::sqrt((sinSq > 0.0) ? sinSq : 0.0);
    const double hx = std::cos(phi) * sinTheta;
    const double hy = std::sin(phi) * sinTheta;
    const double hz = cosTheta;
    const double vDotHSigned = viewX * hx + nDotV * hz;
    double lx = 2.0 * vDotHSigned * hx - viewX;
    double ly = 2.0 * vDotHSigned * hy;
    double lz = 2.0 * vDotHSigned * hz - nDotV;
    const double length = std::sqrt(lx * lx + ly * ly + lz * lz);
    lz = (length > 0.0) ? (lz / length) : 0.0;
    const double nDotL = (lz > 0.0) ? lz : 0.0;
    const double nDotH = (hz > 0.0) ? hz : 0.0;
    const double vDotH = (vDotHSigned > 0.0) ? vDotHSigned : 0.0;
    if (nDotL > 0.0) {
      const double geometry = schlick_ggx(nDotV, k) * schlick_ggx(nDotL, k);
      const double denominator = nDotH * nDotV;
      const double visibility =
          (geometry * vDotH) / ((denominator > 0.0001) ? denominator : 0.0001);
      const double fresnel = std::pow(1.0 - vDotH, 5.0);
      sum.scale += (1.0 - fresnel) * visibility;
      sum.bias += fresnel * visibility;
    }
  }
  sum.scale /= static_cast<double>(sampleCount);
  sum.bias /= static_cast<double>(sampleCount);
  return sum;
}

/// Draws the baked lookup onto the back buffer one texel to a pixel, at
/// the window's top-left, and reads the frame back. It goes through the
/// device's present rather than a pipeline frame, whose own present pass
/// would clear what was drawn.
bool read_back_lookup(int lutSize, CapturedFrame *out) noexcept {
  namespace r = engine::renderer;
  const r::BackendState &backend = r::backend_state();
  const r::RenderDevice *dev = r::render_device();
  const char *path = "brdf_lut_values.tga";
  std::error_code ec{};
  std::filesystem::remove(path, ec);

  dev->bind_render_target(r::kBackBufferTarget);
  dev->set_viewport(0, 0, lutSize, lutSize);
  dev->clear(r::ClearFlags::ColorDepth, 0.0F, 0.0F, 0.0F, 1.0F);
  dev->apply_render_state(r::RenderState{r::DepthTest::Disabled, true,
                                         r::BlendMode::Disabled,
                                         r::CullMode::None});
  dev->bind_program(backend.presentBlitProgram);
  dev->bind_texture_slot(0U, backend.brdfLutTexture);
  if (backend.presentBlitInputLoc.valid()) {
    dev->set_param_i32(backend.presentBlitInputLoc, 0);
  }
  dev->draw(backend.emptyGeometry, r::PrimitiveTopology::Triangles, 0, 3);
  dev->bind_texture_slot(0U, r::kInvalidDeviceTexture);
  dev->bind_program(r::kInvalidDeviceProgram);

  if (!r::render_device_bgfx_request_screenshot(path)) {
    return false;
  }
  // Counted in presents, never in time: the pixels arrive a frame or two
  // after the one they were requested on.
  for (int frame = 0; frame < 16; ++frame) {
    r::present_render_device();
    if (std::filesystem::exists(path, ec) &&
        engine::tests::load_captured_tga(path, out)) {
      return true;
    }
  }
  return false;
}

int run(engine::EnginePipeline &pipeline, engine::runtime::World &) noexcept {
  if (!engine::tests::settle_frames(pipeline, 2)) {
    return 10;
  }
  const engine::renderer::BackendState &backend =
      engine::renderer::backend_state();
  const int lutSize = backend.brdfLutSize;
  if ((backend.brdfLutTexture == engine::renderer::kInvalidDeviceTexture) ||
      (backend.presentBlitProgram == engine::renderer::kInvalidDeviceProgram) ||
      (lutSize < 64)) {
    std::fprintf(stderr, "FAIL: the renderer baked no BRDF lookup\n");
    return 11;
  }
  CapturedFrame frame{};
  if (!read_back_lookup(lutSize, &frame)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  const auto size = static_cast<std::uint32_t>(lutSize);
  if ((frame.width < size) || (frame.height < size)) {
    std::fprintf(stderr, "FAIL: a %ux%u window cannot show a %d-texel lookup\n",
                 frame.width, frame.height, lutSize);
    return 12;
  }

  int result = 0;
  constexpr double kAngles[] = {0.02, 0.05, 0.1};
  for (const double angle : kAngles) {
    // The column whose texel centre is nearest below the angle; its centre
    // is the NdotV the bake evaluated.
    const auto column = static_cast<std::uint32_t>(angle * lutSize);
    const double nDotV = (static_cast<double>(column) + 0.5) / lutSize;

    double worstScale = 0.0;
    double worstBias = 0.0;
    double worstConvergence = 0.0;
    double worstBaked = 0.0;
    int worstRise = 0;
    int worstEnergy = 0;
    int blueSeen = 0;
    int previousBias = 255;
    for (std::uint32_t row = 0U; row < size; ++row) {
      const double roughness = (static_cast<double>(row) + 0.5) / lutSize;
      const SplitSum reference = integrate_split_sum(nDotV, roughness, 512U);
      const int scale = frame.channel(column, row, 2U);
      const int bias = frame.channel(column, row, 1U);
      blueSeen += frame.channel(column, row, 0U);

      const double scaleError = std::fabs(scale - 255.0 * reference.scale);
      const double biasError = std::fabs(bias - 255.0 * reference.bias);
      worstScale = (scaleError > worstScale) ? scaleError : worstScale;
      worstBias = (biasError > worstBias) ? biasError : worstBias;
      worstRise = ((bias - previousBias) > worstRise) ? (bias - previousBias)
                                                      : worstRise;
      previousBias = bias;
      worstEnergy =
          ((scale + bias - 255) > worstEnergy) ? (scale + bias - 255)
                                               : worstEnergy;

      // The converged integral costs 128 times the bake's sum, so it is
      // taken at sixteen rows spread down the column.
      if ((row % (size / 16U)) == 0U) {
        const SplitSum converged =
            integrate_split_sum(nDotV, roughness, 65536U);
        const double scaleGap = std::fabs(reference.scale - converged.scale);
        const double biasGap = std::fabs(reference.bias - converged.bias);
        worstConvergence =
            (scaleGap > worstConvergence) ? scaleGap : worstConvergence;
        worstConvergence =
            (biasGap > worstConvergence) ? biasGap : worstConvergence;
        const double bakedScaleGap = std::fabs(scale / 255.0 - converged.scale);
        const double bakedBiasGap = std::fabs(bias / 255.0 - converged.bias);
        worstBaked = (bakedScaleGap > worstBaked) ? bakedScaleGap : worstBaked;
        worstBaked = (bakedBiasGap > worstBaked) ? bakedBiasGap : worstBaked;
      }
    }
    std::printf("brdf_lut_values_gpu_test: NdotV %.4f (column %u of %d): "
                "scale off the shader's sum by at most %.3f levels, bias "
                "%.3f; baked within %.4f of converged (the sum itself "
                "%.4f); bias rises at most %d, scale + bias over one by %d\n",
                nDotV, column, lutSize, worstScale, worstBias, worstBaked,
                worstConvergence, worstRise, worstEnergy);

    // Half a level is the readback's rounding. The other half covers the
    // GPU's single-precision sum of 512 terms and the half-float target,
    // whose step below one is 2^-11, a fortieth of a level.
    if ((worstScale > 1.0) || (worstBias > 1.0)) {
      std::fprintf(stderr, "FAIL: NdotV %.4f: the baked lookup is %.3f "
                           "(scale) and %.3f (bias) levels from the "
                           "reference sum\n",
                   nDotV, worstScale, worstBias);
      result = 20;
    }
    // A Hammersley set of 512 has a discrepancy near log(N)/N, 0.012, and
    // a well laid out sum lands inside it: 0.01 is two and a half 8-bit
    // levels of a full-white reflection. The baked texel is held to the
    // same bound plus the two thousandths its 8-bit readback rounds away.
    if ((worstConvergence > 0.01) || (worstBaked > 0.012)) {
      std::fprintf(stderr, "FAIL: NdotV %.4f: the baked lookup is %.4f from "
                           "the converged integral, the 512-sample sum "
                           "%.4f\n",
                   nDotV, worstBaked, worstConvergence);
      result = 21;
    }
    // Exact on integers: the true bias falls strictly, so its rounding
    // never rises, and the two terms partition at most the whole of the
    // reflected energy.
    if ((worstRise > 0) || (worstEnergy > 0)) {
      std::fprintf(stderr, "FAIL: NdotV %.4f: bias rises by %d levels with "
                           "roughness, scale + bias passes one by %d\n",
                   nDotV, worstRise, worstEnergy);
      result = 22;
    }
    if (blueSeen != 0) {
      std::fprintf(stderr, "FAIL: NdotV %.4f: the copy is not the two-channel "
                           "lookup (blue sums to %d)\n",
                   nDotV, blueSeen);
      result = 23;
    }
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("brdf_lut_values_gpu_test", &run);
}
