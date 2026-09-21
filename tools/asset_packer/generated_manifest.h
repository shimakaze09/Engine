// Declares the packer's check of generated sources against the
// generated.manifest their generator publishes last: the
// manifest is the consumer commit boundary, so a source whose bytes
// disagree with it belongs to an interrupted publish and is never cooked.

#pragma once

#include <cstddef>
#include <cstdint>

/// True when `path` may be cooked: its directory has no manifest, the
/// manifest does not list the file, or it lists exactly these bytes
/// (size and FNV-1a 64 content hash). False, with a diagnostic in
/// `message`, when the manifest lists different bytes (a torn publish) or
/// cannot be read (a torn manifest write).
bool generated_source_certified(const char *path, std::uint64_t contentHash,
                                char *message, std::size_t messageCapacity);
