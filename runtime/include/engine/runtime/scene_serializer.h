// Declares scene serializer types and APIs for the Engine runtime world.

#pragma once

#include <cstddef>
#include <memory>

#include "engine/core/file_read.h"
#include "engine/core/validation_report.h"

namespace engine::content {
struct AssetCatalog;
} // namespace engine::content

namespace engine::runtime {

class World;

/// Callback invoked immediately before a scene transition destructively
/// replaces (load_scene) or clears (reset_world) the live world's content,
/// while the outgoing entities and their script modules are still alive
/// and callable; used by process_pending_scene_op to dispatch on_end_play
/// to the outgoing scene before teardown. Never called on a failed
/// load — a failure leaves the outgoing world untouched and unreported.
using SceneTeardownHook = void (*)() noexcept;

/// Live world state the scene format cannot represent: counts of
/// provenance-free convex-hull payloads, heightfield payloads, and active
/// physics joints. A save with any nonzero count is refused before the
/// destination file or buffer is touched, so a success result is never
/// lossy; callers use the counts to compose actionable diagnostics.
struct SceneSaveBlockers final {
  std::size_t customHullPayloads = 0U;
  std::size_t heightfieldPayloads = 0U;
  std::size_t activeJoints = 0U;
};

/// Collects the live state save_scene would refuse to drop.
SceneSaveBlockers collect_scene_save_blockers(const World &world) noexcept;

/// Saves the world's scene to `path` through a staged atomic replace, one
/// field per line (core::JsonLayout::Lines) so a diff or a merge works per
/// field. A path under a mounted virtual prefix ("assets/level.scene")
/// names the mounted file wherever the process was started; any other path
/// is an OS path, used as it is.
bool save_scene(const World &world, const char *path) noexcept;
/// Saves the scene into `buffer` as compact JSON; false, with an Error,
/// when it needs more than `capacity` bytes.
bool save_scene(const World &world, char *buffer, std::size_t capacity,
                std::size_t *outSize) noexcept;
/// Saves the scene as compact JSON into a buffer sized to it, for
/// in-memory snapshots no person reads (the Play snapshot): serialized
/// once, never retried for room, so it fails only when the document
/// cannot be produced at all.
bool save_scene(const World &world, std::unique_ptr<char[]> *outBuffer,
                std::size_t *outSize) noexcept;
/// Fingerprints the document at `path`, resolved as save_scene resolves
/// it, so an editor can tell whether the file changed on disk since it
/// loaded or wrote it (core::file_fingerprint).
core::FileReadResult document_fingerprint(const char *path,
                                          core::FileFingerprint *out) noexcept;
/// Loads the scene at `path` (resolved as save_scene resolves it) into
/// the world. A malformed document is
/// refused with the world untouched; a well-formed one whose references
/// do not resolve still loads, and each such reference is a Warning in
/// *outReport (when given) and a logged diagnostic: `dangling_parent`
/// (a Transform parent no entity carries; the child loads as a root),
/// `missing_script` and `missing_controller` (a path under a mounted
/// prefix that names no file; unmounted prefixes are not judged). A key no
/// reader looks up is `unknown_key`, keyed by its path
/// ("entities[3].components.Colider"); the next save would drop it.
bool load_scene(World &world, const char *path,
                SceneTeardownHook beforeTeardown = nullptr,
                core::ValidationReport *outReport = nullptr) noexcept;
/// Loads the scene held in `buffer`; same contract as the path form.
bool load_scene(World &world, const char *buffer, std::size_t size,
                SceneTeardownHook beforeTeardown = nullptr,
                core::ValidationReport *outReport = nullptr) noexcept;
/// Checks every asset reference the world's components carry — a mesh's
/// mesh and material, a sky light's environment, a foliage patch's LOD
/// meshes — against `catalog`, and records each one that names no
/// catalogued asset as a Warning in *report (when given) and a logged
/// diagnostic, as load_scene records a path that names no file:
/// `missing_mesh`, `missing_material`, `missing_environment`, or
/// `wrong_asset_type` for one naming an asset of another type. The world
/// is not changed. The runtime draws such an entity without the asset;
/// engine_validate, which catalogues a project as the engine does, fails
/// on any finding.
void validate_scene_asset_references(const World &world,
                                     const content::AssetCatalog &catalog,
                                     core::ValidationReport *report) noexcept;
/// Resets this object back to its reusable empty state for world.
void reset_world(World &world,
                 SceneTeardownHook beforeTeardown = nullptr) noexcept;

} // namespace engine::runtime
