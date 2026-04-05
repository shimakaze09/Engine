#include "engine/engine.h"

int main() {
    if (!engine::bootstrap()) {
        return 1;
    }

    engine::run(10U);
    engine::shutdown();
    return 0;
}
