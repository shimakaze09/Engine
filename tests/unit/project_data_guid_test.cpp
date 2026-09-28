// Verifies per-project data named by a project's GUID
// (set_project_data_guid): the directory is projects/<32 hex digits of
// the GUID> under the platform save directory, the same GUID always names
// the same directory, a GUID replaces a root-named project and a root
// replaces a GUID, the nil GUID is refused and leaves no project named,
// and a root-named directory keeps its 16-digit form, so the two can never
// name one directory. Core startup names the project by the config's GUID
// ahead of its content root.

#include "engine/core/project_data.h"

#include <cstring>
#include <string>

#include "../test_harness.h"
#include "engine/core/bootstrap.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"

namespace {

namespace core = engine::core;

std::string data_dir() {
  char directory[1024] = {};
  if (!core::project_data_dir(directory, sizeof(directory))) {
    return {};
  }
  return directory;
}

std::string projects_prefix() {
  char saveDir[1024] = {};
  if (!core::platform_get_save_dir(saveDir, sizeof(saveDir))) {
    return {};
  }
  return std::string(saveDir) + "/projects/";
}

} // namespace

int main() {
  engine::tests::TestContext t;
  t.check(core::initialize_logging(), "initialize logging");
  const std::string prefix = projects_prefix();
  t.check(!prefix.empty(), "the platform save directory resolves");

  const core::AssetGuid guid{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
  t.check(core::set_project_data_guid(guid) && core::project_data_named(),
          "a valid GUID names the project");
  const std::string byGuid = data_dir();
  t.check(byGuid == prefix + "0123456789abcdeffedcba9876543210",
          "the directory is the GUID's 32 hex digits, high half first");
  t.check(core::set_project_data_guid(guid) && (data_dir() == byGuid),
          "the same GUID names the same directory every time");

  const core::AssetGuid other{0x0123456789abcdefULL, 0x0ULL};
  t.check(core::set_project_data_guid(other) && (data_dir() != byGuid),
          "a GUID differing only in its low half names another directory");

  t.check(core::set_project_data_root("."), "a root names the project");
  const std::string byRoot = data_dir();
  t.check((byRoot.size() == prefix.size() + 16U) &&
              (byRoot.compare(0U, prefix.size(), prefix) == 0),
          "a root-named directory keeps its 16 hex digits");
  t.check(core::set_project_data_guid(guid) && (data_dir() == byGuid),
          "a GUID replaces a root-named project");
  t.check(core::set_project_data_root(".") && (data_dir() == byRoot),
          "a root replaces a GUID-named project");

  t.check(core::set_project_data_guid(guid), "name the GUID again");
  t.check(!core::set_project_data_guid(core::kNilAssetGuid) &&
              !core::project_data_named() && data_dir().empty(),
          "the nil GUID is refused and leaves no project named");

  core::clear_project_data_root();
  core::shutdown_logging();

  // Core startup names the project by the GUID when the config carries
  // one, even beside a content root, and shutdown forgets it.
  core::CoreConfig config{};
  config.initializePlatform = false;
  config.workerThreads = 1U;
  config.projectRoot = ".";
  config.projectGuid = guid;
  t.check(core::initialize_core(config), "core initializes");
  t.check(data_dir() == byGuid,
          "core startup prefers the GUID over the content root");
  core::shutdown_core();
  t.check(!core::project_data_named(), "core shutdown forgets the project");
  return t.finish("project_data_guid");
}
