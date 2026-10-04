// Validates authored content from the command line: the engine's content,
// the project's content and its packages are mounted and catalogued as
// the engine catalogues them at boot, each scene loads through the
// production loader, and every asset reference it carries is checked
// against that catalog. Every catalogued prefab, material and animation
// controller is then loaded through its own production loader, as
// Unreal's data validation checks every asset type and not only maps: a
// prefab's references are checked as a scene's are, and a material or a
// controller naming a parent, texture or clip that does not resolve does
// not load. Every finding is printed one per line, and the exit code is
// non-zero when a mount does not index cleanly, a scene fails to load, or
// any document reports a finding, so CI catches a dangling reference
// before an author does. Given --project, it opens the project through
// engine::open_project and validates every scene the project lists.
//
// With --bake-navmesh each loaded scene's NavMeshSurfaces are baked and
// written to their .navmesh files through the editor's own bake path
// (runtime::write_nav_mesh_surface_file), so a build bakes what the
// editor's Bake button would; with --check-navmesh each file must hold
// exactly what that bake writes now, so a build refuses a mesh its level
// has moved away from.
//
// With --list-dependencies each scene's dependency closure is printed, one
// "dependency <scene> <asset>" line per asset it needs through the
// catalog's edges (content::collect_asset_closure): what a package of the
// scene would carry.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "engine/content/asset_catalog.h"
#include "engine/content/asset_references.h"
#include "engine/core/command_line.h"
#include "engine/core/logging.h"
#include "engine/core/validation_report.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"
#include "engine/project.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"
#include "engine/runtime/animation_system.h"
#include "engine/runtime/content_catalog.h"
#include "engine/runtime/nav_mesh_surface_file.h"
#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

constexpr engine::core::CommandLineOption kOptions[] = {
    {"assets", engine::core::CommandLineOptionKind::Value},
    {"project", engine::core::CommandLineOptionKind::Value},
    {"bake-navmesh", engine::core::CommandLineOptionKind::Flag},
    {"check-navmesh", engine::core::CommandLineOptionKind::Flag},
    {"list-dependencies", engine::core::CommandLineOptionKind::Flag},
};

/// What a run does with each scene's navigation surfaces.
enum class NavMeshMode : std::uint8_t { None, Bake, Check };

void print_usage() {
  std::fprintf(stderr,
               "usage: engine_validate [--bake-navmesh | --check-navmesh] "
               "[--list-dependencies] [--assets <dir>] <scene.json>...\n"
               "       engine_validate [--bake-navmesh | --check-navmesh] "
               "[--list-dependencies] --project <dir or .project> "
               "[<scene.json>...]\n");
}

const char *nav_mesh_write_text(engine::runtime::NavMeshWriteResult result) {
  using engine::runtime::NavMeshWriteResult;
  switch (result) {
  case NavMeshWriteResult::Written:
    return "written";
  case NavMeshWriteResult::BadPath:
    return "its path does not end in .navmesh";
  case NavMeshWriteResult::BakeFailed:
    return "the bake failed";
  case NavMeshWriteResult::Empty:
    return "nothing walkable inside the volume; nothing was written";
  case NavMeshWriteResult::EncodeFailed:
    return "the baked mesh could not be encoded";
  case NavMeshWriteResult::WriteFailed:
    return "the file could not be written; the previous file is kept";
  }
  return "unknown";
}

const char *nav_mesh_state_text(engine::runtime::NavMeshFileState state) {
  using engine::runtime::NavMeshFileState;
  switch (state) {
  case NavMeshFileState::Current:
    return "current";
  case NavMeshFileState::Missing:
    return "missing: bake the surface to write it";
  case NavMeshFileState::Stale:
    return "stale: the level or the surface changed since it was baked";
  case NavMeshFileState::Unreadable:
    return "unreadable";
  case NavMeshFileState::BadPath:
    return "its path does not end in .navmesh";
  case NavMeshFileState::BakeFailed:
    return "the bake failed or found nothing walkable";
  }
  return "unknown";
}

/// Bakes, or checks, every NavMeshSurface of the scene loaded in `world`,
/// printing one line per surface; returns the surfaces that failed. A
/// surface never baked has no path to write to, and this tool never edits
/// the scene to give it one, so that is a finding too.
int process_nav_mesh_surfaces(engine::runtime::World &world, const char *scene,
                              NavMeshMode mode) {
  // The bake places each surface at its entity's composed position, as
  // the editor's World has it: one transform phase composes them, and the
  // World is back in Input for the next scene.
  world.begin_transform_phase();
  world.end_frame_phase();
  int failures = 0;
  const std::size_t count = world.nav_mesh_surface_count();
  for (std::size_t i = 0U; i < count; ++i) {
    const engine::runtime::Entity entity = world.nav_mesh_surface_entity_at(i);
    const engine::runtime::NavMeshSurfaceComponent *surface =
        world.nav_mesh_surface_at(i);
    if (surface == nullptr) {
      continue;
    }
    const unsigned long long id =
        static_cast<unsigned long long>(world.persistent_id(entity));
    if (surface->navMeshPath[0] == '\0') {
      std::printf("%s: error: nav_mesh_never_baked (entity %llu): bake it "
                  "in the editor once to name its file\n",
                  scene, id);
      ++failures;
      continue;
    }
    if (mode == NavMeshMode::Bake) {
      const engine::runtime::NavMeshWriteReport report =
          engine::runtime::write_nav_mesh_surface_file(world, entity,
                                                       surface->navMeshPath);
      const bool written =
          report.result == engine::runtime::NavMeshWriteResult::Written;
      if (written) {
        std::printf("%s: baked %zu polygons into %s (entity %llu)%s\n", scene,
                    report.polygons, surface->navMeshPath, id,
                    report.identified ? ""
                                      : "; its sidecar could not be written");
      } else {
        std::printf("%s: error: nav_mesh_not_baked %s (entity %llu): %s\n",
                    scene, surface->navMeshPath, id,
                    nav_mesh_write_text(report.result));
      }
      failures += (written && report.identified) ? 0 : 1;
    } else {
      const engine::runtime::NavMeshFileState state =
          engine::runtime::check_nav_mesh_surface_file(world, entity,
                                                       surface->navMeshPath);
      const bool current = state == engine::runtime::NavMeshFileState::Current;
      std::printf("%s: %s%s %s (entity %llu): %s\n", scene,
                  current ? "" : "error: ",
                  current ? "nav_mesh_current" : "nav_mesh_out_of_date",
                  surface->navMeshPath, id, nav_mesh_state_text(state));
      failures += current ? 0 : 1;
    }
  }
  return failures;
}

/// Loads one scene, checks its asset references against `catalog` and
/// prints its findings; returns the number of findings, or 1 when the
/// scene did not load at all. Every finding counts: each is a reference
/// that resolves to nothing, which the runtime survives but an author
/// must fix.
int validate_scene(engine::runtime::World &world,
                   const engine::content::AssetCatalog &catalog,
                   const char *path, NavMeshMode navMeshMode) {
  engine::core::ValidationReport report{};
  if (!engine::runtime::load_scene(world, path, nullptr, &report)) {
    std::printf("%s: error: scene did not load\n", path);
    return 1;
  }
  engine::runtime::validate_scene_asset_references(world, catalog, &report);
  std::size_t skipped = 0U;
  for (std::size_t i = 0U; i < report.count; ++i) {
    const engine::core::ValidationEntry &entry = report.entries[i];
    // A bake writes the files this names; whether it did is reported per
    // surface below, so the load's note that they are missing is not a
    // finding of its own.
    if ((navMeshMode == NavMeshMode::Bake) &&
        (std::strcmp(entry.code, "missing_nav_mesh") == 0)) {
      ++skipped;
      continue;
    }
    std::printf("%s: %s: %s %s (entity %u)\n", path,
                (entry.severity == engine::core::ValidationSeverity::Error)
                    ? "error"
                    : "warning",
                entry.code, entry.key, entry.entityPersistentId);
  }
  if (report.dropped > 0U) {
    std::printf("%s: warning: %zu further finding(s) not listed\n", path,
                report.dropped);
  }
  const std::size_t findings = report.count + report.dropped - skipped;
  std::printf("%s: %zu finding(s)\n", path, findings);
  const int navMeshFailures =
      (navMeshMode == NavMeshMode::None)
          ? 0
          : process_nav_mesh_surfaces(world, path, navMeshMode);
  return static_cast<int>(findings) + navMeshFailures;
}

/// The first Error a loader logs while one document is checked: why it did
/// not load, which the finding repeats so the author need not search the
/// log for it.
struct LoadReason final {
  bool captured = false;
  char text[256] = {};
};

void capture_load_reason(engine::core::LogLevel level, const char *,
                         const char *message, void *userData) noexcept {
  auto *reason = static_cast<LoadReason *>(userData);
  if ((level != engine::core::LogLevel::Error) || reason->captured ||
      (message == nullptr)) {
    return;
  }
  reason->captured = true;
  std::snprintf(reason->text, sizeof(reason->text), "%s", message);
}

/// Prints a document that did not load as one finding under `code`, with
/// the loader's own reason; returns the finding count.
int report_unloadable(const char *path, const char *code,
                      const LoadReason &reason) {
  std::printf("%s: error: %s %s\n", path, code,
              reason.captured ? reason.text : "(no reason logged)");
  std::printf("%s: 1 finding(s)\n", path);
  return 1;
}

/// Loads every catalogued prefab, material and animation controller
/// through its production loader and returns the findings. A prefab is
/// instantiated into `world` and its references checked as a scene's are
/// (`unloadable_prefab` when it does not instantiate); a material loads
/// into a scratch asset database, which refuses a parent or texture the
/// catalog does not hold (`unloadable_material`); a controller loads with
/// its clips (`unloadable_controller`).
int validate_catalogued_documents(engine::runtime::World &world,
                                  engine::content::AssetCatalog &catalog) {
  using engine::content::AssetTypeTag;
  const std::unique_ptr<engine::renderer::AssetDatabase> materials(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (materials == nullptr) {
    std::printf("error: could not allocate an asset database\n");
    return 1;
  }
  // The catalog grows while materials load (a parent is registered as it
  // loads), so the documents are listed first and checked after.
  struct Document final {
    AssetTypeTag type = AssetTypeTag::Unknown;
    char path[sizeof(engine::content::AssetMetadata::filePath)] = {};
  };
  std::vector<Document> documents;
  const std::size_t recordCount =
      engine::content::asset_catalog_record_count(&catalog);
  for (std::size_t i = 0U; i < recordCount; ++i) {
    const engine::content::AssetMetadata *record =
        engine::content::asset_catalog_record(&catalog, i);
    if ((record == nullptr) ||
        ((record->typeTag != AssetTypeTag::Prefab) &&
         (record->typeTag != AssetTypeTag::Material) &&
         (record->typeTag != AssetTypeTag::AnimationController))) {
      continue;
    }
    Document document{};
    document.type = record->typeTag;
    std::memcpy(document.path, record->filePath.data(), sizeof(document.path));
    document.path[sizeof(document.path) - 1U] = '\0';
    documents.push_back(document);
  }
  // By path, so the findings read the same on every run and every OS.
  std::sort(documents.begin(), documents.end(),
            [](const Document &a, const Document &b) {
              return std::strcmp(a.path, b.path) < 0;
            });
  std::size_t counts[3] = {};
  for (const Document &document : documents) {
    ++counts[(document.type == AssetTypeTag::Prefab)     ? 0U
             : (document.type == AssetTypeTag::Material) ? 1U
                                                         : 2U];
  }
  std::printf("checking %zu prefab(s), %zu material(s) and %zu animation "
              "controller(s)\n",
              counts[0], counts[1], counts[2]);

  int failures = 0;
  for (const Document &document : documents) {
    LoadReason reason{};
    static_cast<void>(
        engine::core::log_register_sink(&capture_load_reason, &reason));
    if (document.type == AssetTypeTag::Prefab) {
      world.reset_all_entities();
      const bool loaded =
          engine::runtime::instantiate_prefab(world, document.path) !=
          engine::runtime::kInvalidEntity;
      engine::core::log_unregister_sink(&capture_load_reason, &reason);
      if (!loaded) {
        failures +=
            report_unloadable(document.path, "unloadable_prefab", reason);
        continue;
      }
      engine::core::ValidationReport report{};
      engine::runtime::validate_scene_asset_references(world, catalog, &report);
      for (std::size_t i = 0U; i < report.count; ++i) {
        const engine::core::ValidationEntry &entry = report.entries[i];
        std::printf("%s: %s: %s %s\n", document.path,
                    (entry.severity == engine::core::ValidationSeverity::Error)
                        ? "error"
                        : "warning",
                    entry.code, entry.key);
      }
      const std::size_t findings = report.count + report.dropped;
      std::printf("%s: %zu finding(s)\n", document.path, findings);
      failures += static_cast<int>(findings);
    } else if (document.type == AssetTypeTag::Material) {
      const bool loaded = engine::renderer::load_material_asset(
                              materials.get(), &catalog, document.path)
                              .has_value();
      engine::core::log_unregister_sink(&capture_load_reason, &reason);
      failures += loaded ? 0
                         : report_unloadable(document.path,
                                             "unloadable_material", reason);
    } else {
      const bool loaded =
          engine::runtime::acquire_anim_controller(document.path) !=
          engine::runtime::kInvalidAnimSlot;
      engine::core::log_unregister_sink(&capture_load_reason, &reason);
      failures += loaded ? 0
                         : report_unloadable(document.path,
                                             "unloadable_controller", reason);
    }
  }
  world.reset_all_entities();
  engine::runtime::reset_anim_controllers();
  return failures;
}

} // namespace

/// Runs this executable or test program.
/// Prints every asset the scene at `virtualPath` needs, one
/// "dependency <scene> <asset>" line each, from the catalog's edges
/// (content::collect_asset_closure): what a package of the scene carries.
void print_dependencies(const engine::content::AssetCatalog &catalog,
                        const char *virtualPath) {
  const engine::content::AssetId scene =
      engine::content::make_asset_id_from_path(virtualPath);
  const std::size_t count =
      engine::content::collect_asset_closure(&catalog, scene, nullptr, 0U);
  std::vector<engine::content::AssetId> ids(count);
  static_cast<void>(engine::content::collect_asset_closure(
      &catalog, scene, ids.data(), ids.size()));
  for (const engine::content::AssetId id : ids) {
    const engine::content::AssetMetadata *record =
        engine::content::find_asset_metadata(&catalog, id);
    if (record != nullptr) {
      std::printf("dependency %s %s\n", virtualPath, record->filePath.data());
    } else {
      std::printf("dependency %s %016llx (not catalogued)\n", virtualPath,
                  static_cast<unsigned long long>(id));
    }
  }
}

/// The catalog's virtual path for the scene file `osPath` under the assets
/// directory, written to `out`; false when it is outside it.
bool scene_virtual_path(const engine::EngineConfig &config, const char *osPath,
                        char *out, std::size_t capacity) {
  const std::size_t rootLength = std::strlen(config.assetRoot);
  if (std::strncmp(osPath, config.assetRoot, rootLength) != 0) {
    return false;
  }
  const char *relative = osPath + rootLength;
  while ((*relative == '/') || (*relative == '\\')) {
    ++relative;
  }
  const int written =
      std::snprintf(out, capacity, "%s/%s", config.assetMount, relative);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

int main(int argc, char **argv) {
  const auto commandLine = engine::core::parse_command_line(
      argc, argv, kOptions, sizeof(kOptions) / sizeof(kOptions[0]),
      engine::core::kMaxCommandLinePositionals);
  if (!commandLine.has_value()) {
    const engine::core::CommandLineFailure failure = commandLine.error();
    std::fprintf(stderr, "error: %s: %s\n",
                 (failure.argumentIndex > 0) ? argv[failure.argumentIndex]
                                             : "engine_validate",
                 engine::core::command_line_failure_text(failure.kind));
    print_usage();
    return 2;
  }
  const bool byProject = commandLine->has("project");
  if ((byProject && commandLine->has("assets")) ||
      (!byProject && (commandLine->positional_count() == 0U)) ||
      (commandLine->has("bake-navmesh") && commandLine->has("check-navmesh"))) {
    print_usage();
    return 2;
  }
  const NavMeshMode navMeshMode =
      commandLine->has("bake-navmesh")    ? NavMeshMode::Bake
      : commandLine->has("check-navmesh") ? NavMeshMode::Check
                                          : NavMeshMode::None;
  const bool listDependencies = commandLine->has("list-dependencies");
  const char *assetsDirectory =
      commandLine->has("assets") ? commandLine->value("assets") : "assets";
  // Static: about 18 KB, and it must outlive the mount that points at it.
  static engine::ProjectStorage project{};
  // What the run of this content would mount: the project's own when one
  // is named, the given assets directory otherwise.
  engine::EngineConfig config{};
  config.assetRoot = assetsDirectory;
  if (byProject) {
    const auto opened =
        engine::open_project(commandLine->value("project"), &project, &config);
    if (!opened.has_value()) {
      std::fprintf(stderr, "error: %s: %s\n", commandLine->value("project"),
                   engine::project_open_failure_text(opened.error().kind));
      return 2;
    }
  }

  engine::runtime::ensure_runtime_reflection_registered();
  if (!engine::core::initialize_logging() || !engine::core::initialize_vfs()) {
    std::fprintf(stderr, "error: could not start logging and the VFS\n");
    return 2;
  }
  // Engine content that cannot be found is not fatal (a project need not
  // name any), but its references will not resolve, and that is said once.
  char engineRoot[1024] = {};
  engine::resolve_engine_root(engineRoot, sizeof(engineRoot));
  if (engine::core::os_directory_exists(engineRoot)) {
    config.engineRoot = engineRoot;
  } else {
    std::printf("note: engine content not found at %s (set ENGINE_ROOT); "
                "references to it will not resolve\n",
                engineRoot);
  }
  // Mounted as the engine mounts them, so a path a scene names (a script,
  // a controller) is judged against the same tree.
  bool mounted = (config.engineRoot[0] == '\0') ||
                 engine::core::mount(config.engineMount, config.engineRoot);
  mounted = mounted && engine::core::mount(config.assetMount, config.assetRoot);
  for (std::size_t i = 0U; mounted && (i < config.packageCount); ++i) {
    mounted =
        engine::core::mount(config.packages[i].mount, config.packages[i].root);
  }
  if (!mounted) {
    std::fprintf(stderr, "error: could not mount %s as %s\n", config.assetRoot,
                 config.assetMount);
    return 2;
  }
  // Catalogued as the engine catalogues them at boot, so an identity
  // resolves here exactly as it does in the engine.
  const std::unique_ptr<engine::content::AssetCatalog> catalog =
      engine::runtime::create_asset_catalog();
  if (catalog == nullptr) {
    std::fprintf(stderr, "error: could not allocate an asset catalog\n");
    return 2;
  }
  int failures = 0;
  if (!engine::runtime::catalogue_engine_content(catalog.get(), config)) {
    std::printf("error: a content mount does not index cleanly\n");
    failures = 1;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    std::fprintf(stderr, "error: could not allocate a world\n");
    return 2;
  }

  if (byProject) {
    // The document lists virtual paths under the assets mount; the loader
    // reads OS paths, so each is taken from the content root.
    const std::size_t mountLength = std::strlen("assets/");
    for (std::size_t i = 0U; i < project.document.sceneCount; ++i) {
      char osPath[engine::kProjectOsPathCapacity * 2U] = {};
      std::snprintf(osPath, sizeof(osPath), "%s/%s", project.contentRoot,
                    project.document.scenes[i] + mountLength);
      failures += validate_scene(*world, *catalog, osPath, navMeshMode);
      if (listDependencies) {
        print_dependencies(*catalog, project.document.scenes[i]);
      }
    }
  }
  for (std::size_t i = 0U; i < commandLine->positional_count(); ++i) {
    failures += validate_scene(*world, *catalog, commandLine->positional(i),
                               navMeshMode);
    char virtualPath[520] = {};
    if (listDependencies &&
        scene_virtual_path(config, commandLine->positional(i), virtualPath,
                           sizeof(virtualPath))) {
      print_dependencies(*catalog, virtualPath);
    }
  }
  failures += validate_catalogued_documents(*world, *catalog);

  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
  return (failures == 0) ? 0 : 1;
}
