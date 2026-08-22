$input v_texcoord0

// Depth-seed pass (#138): copies the G-buffer depth into the bound
// target's depth attachment through a fullscreen draw. Backends without
// a depth blit (bgfx's Vulkan and WebGL paths report
// caps.depthBlit=false) seed the scene target's depth this way before
// the depth-tested sky pass; the pass state masks color writes off and
// forces DepthTest::Always so every texel lands.

#include <bgfx_shader.sh>

SAMPLER2D(uDepth, 0);

void main() {
    gl_FragDepth = texture2D(uDepth, v_texcoord0).r;
    gl_FragColor = vec4_splat(0.0);
}
