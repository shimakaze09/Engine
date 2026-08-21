// Declares the packer's bgfx shader cook mode (#138 Phase C): manifest-
// driven shaderc invocation producing deterministic per-profile shader
// binaries certified by the shared cook-stamp machinery.

#pragma once

/// Entry point for `asset_packer --shader-manifest ...`; returns the
/// process exit code (0 on success or up-to-date skip).
int run_shader_cook(int argc, char **argv);
