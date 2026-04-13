---
description: 'Add a new render pass to the forward pipeline with full resource management and state tracking'
---
Add a new render pass named ${input:passName} to the renderer.

## Current Pipeline State (VERIFY BEFORE STARTING)

Read these files to understand the current pipeline:
1. `renderer/src/command_buffer.cpp` — flush_renderer() orchestrates pass order
2. `renderer/src/pass_resources.cpp` — GPU resource allocation/destruction
3. `renderer/include/engine/renderer/pass_resources.h` — PassResourceId enum
4. `renderer/include/engine/renderer/render_device.h` — function pointer table (NOT raw GL calls)

Current pass order in flush_renderer():
1. **Scene pass**: PBR shader -> HDR FBO (sceneColorTexture + sceneDepthTexture)
2. **Tonemap pass**: fullscreen triangle, reads sceneColor -> LDR FBO (finalColorTexture)
3. **Back buffer**: clear for ImGui overlay

**Known state**: Forward renderer only. No deferred, no shadow maps, no instancing, no post-processing stack. The renderer uses a RenderDevice function pointer table — do NOT use raw glBindFramebuffer etc.

## Implementation Steps

### 1. Pass Resources (if new textures/FBOs needed)
- Add new PassResourceId slot(s) in `pass_resources.h`
- Allocate in `create_gpu_resources()` following the existing pattern
- Destroy in `destroy_gpu_resources()` in REVERSE order of creation
- Handle window resize in `resize_pass_resources()`
- Verify: no resource leaks on resize, no dangling GPU handles

### 2. Shader
- Add shader files in `assets/shaders/${input:passName}.vert` and `${input:passName}.frag`
- Load in `initialize_backend()` in command_buffer.cpp
- On load failure: call `reset_backend_on_failure()`, log error, return false
- Do NOT use hardcoded paths. Use the same path resolution as existing shaders.

### 3. Pass Execution
- Add to `flush_renderer()` in the correct position relative to existing passes
- Bind the correct FBO, viewport, and textures
- Execute draw calls through RenderDevice function pointers
- Restore GL state after the pass: depth mask, blend mode, cull face, viewport

### 4. State Management Rules
- NEVER modify global GL state without restoring it
- NEVER use raw GL calls — always go through render_device()->xxx()
- Check for GL errors in debug builds (the engine has a GL error check mechanism)
- All GPU resources must have matching create/destroy paths

### 5. Integration
- If the pass is optional (e.g., debug visualization), gate it behind a CVar
- If the pass needs per-frame data, ensure it reads from the correct double-buffer phase
- Document the pass in a comment at the top of the implementation

## Do NOT
- Use raw glBindFramebuffer, glUseProgram, etc. — use RenderDevice.
- Leak GPU resources on resize or shutdown.
- Assume deferred rendering exists — it does NOT.
- Add passes that depend on shadow maps — they do NOT exist yet.
