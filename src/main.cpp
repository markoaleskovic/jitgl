#include "core/Engine.h"

int main() {
    // Engine owns all subsystems; Run() blocks until the window is closed.
    if (Engine engine; engine.Init()) {
        engine.Run();
    }
    return 0;
}
