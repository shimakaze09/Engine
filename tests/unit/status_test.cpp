// Pins the failure vocabulary's contract: the success value, a failure's
// category and detail, the explicit bool that keeps `if (!op())` sites
// readable, Degraded counting as success, and one distinct name per
// category.

#include <cstring>
#include <iterator>

#include "../test_harness.h"
#include "engine/core/status.h"

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  using engine::core::FailureKind;
  using engine::core::Status;

  const Status ok = Status::ok();
  ctx.check(ok.succeeded() && static_cast<bool>(ok) && (ok.detail == 0U),
            "the success value succeeds");
  ctx.check(ok == Status{}, "a default status is the success value");

  const Status full = Status::fail(FailureKind::CapacityExhausted, 3U);
  ctx.check(!full.succeeded() && !full && (full.detail == 3U),
            "a failure fails and keeps its detail");
  ctx.check(full != Status::fail(FailureKind::CapacityExhausted),
            "the detail is part of the value");

  ctx.check(Status::fail(FailureKind::Degraded).succeeded(),
            "degraded is a success the caller may inspect");

  constexpr FailureKind kinds[] = {
      FailureKind::Ok,           FailureKind::InvalidArgument,
      FailureKind::NotFound,     FailureKind::Unsupported,
      FailureKind::CapacityExhausted, FailureKind::DataMalformed,
      FailureKind::IoFailed,     FailureKind::Transient,
      FailureKind::InvariantViolated, FailureKind::Degraded};
  for (std::size_t i = 0U; i < std::size(kinds); ++i) {
    const char *name = engine::core::failure_kind_name(kinds[i]);
    ctx.check((name != nullptr) && (name[0] != '\0') &&
                  (std::strcmp(name, "unknown") != 0),
              "every category has a name");
    for (std::size_t j = 0U; j < i; ++j) {
      ctx.check(std::strcmp(name, engine::core::failure_kind_name(kinds[j])) !=
                    0,
                "names are distinct");
    }
  }
  ctx.check(std::strcmp(engine::core::failure_kind_name(
                            static_cast<FailureKind>(200)),
                        "unknown") == 0,
            "a value outside the vocabulary reads as unknown");

  return ctx.finish("status");
}
