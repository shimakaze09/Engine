#include "engine/engine.h"

int main() {
  if (!engine::bootstrap()) {
    return 1;
  }

  engine::run(0);
  engine::shutdown();
  return 0;
}
