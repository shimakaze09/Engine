// Pins the job graph's cycle contract behind #570: add_dependency refuses
// an edge that would close a cycle (and a self edge), so a cyclic graph
// cannot be built through the public API and the dispatch-time acyclicity
// check is defence in depth, never a path content can reach. The Debug
// assert that used to sit on that check is gone; this test documents why
// no regression can make the check itself fail from production code, and
// that a refused edge leaves the graph usable.

#include "engine/core/job_system.h"

#include <cstdio>

namespace {

void noop_job(void *data) noexcept { static_cast<void>(data); }

/// Shuts the job system down before returning a code, so an early exit
/// never leaves the worker threads holding the process open.
int finish(int code) noexcept {
  engine::core::shutdown_job_system();
  return code;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_job_system(1U)) {
    std::fprintf(stderr, "FAIL: job system did not initialize\n");
    return 1;
  }

  engine::core::Job job{};
  job.function = &noop_job;

  // Ending a graph that was never begun is a contract violation the
  // status names, not a silent false.
  if (engine::core::end_frame_graph().kind !=
      engine::core::FailureKind::InvariantViolated) {
    std::fprintf(stderr, "FAIL: ending without a graph is not named\n");
    return finish(7);
  }

  if (!engine::core::begin_frame_graph()) {
    return finish(2);
  }
  const engine::core::JobHandle a = engine::core::submit(job);
  const engine::core::JobHandle b = engine::core::submit(job);
  const engine::core::JobHandle c = engine::core::submit(job);
  if (!engine::core::add_dependency(a, b) ||
      !engine::core::add_dependency(b, c)) {
    std::fprintf(stderr, "FAIL: acyclic edges were refused\n");
    return finish(3);
  }
  // Closing the chain, directly or transitively, and a self edge are all
  // refused at the edge, before the graph could ever hold a cycle.
  if (engine::core::add_dependency(b, a) ||
      engine::core::add_dependency(c, a) ||
      engine::core::add_dependency(a, a)) {
    std::fprintf(stderr, "FAIL: a cycle-closing edge was accepted\n");
    return finish(4);
  }

  // The refusals leave the graph intact: it dispatches and completes.
  // Completion is read before end_frame_graph, which retires the handles.
  engine::core::wait_all();
  if (!engine::core::is_completed(c)) {
    std::fprintf(stderr, "FAIL: the graph did not complete after refusals\n");
    return finish(5);
  }
  if (!engine::core::end_frame_graph()) {
    std::fprintf(stderr, "FAIL: the graph did not end cleanly\n");
    return finish(6);
  }

  std::puts("job_graph_cycle_test passed");
  return finish(0);
}
