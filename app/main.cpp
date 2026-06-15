// Implements main behavior for the Engine editor application entry point.

#include "engine/engine.h"

/// Runs this executable or test program.
int main() {
  if (!engine::bootstrap()) {
    return 1;
  }

  engine::run(0);
  engine::shutdown();
  return 0;
}
