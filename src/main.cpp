#include "core/Engine.h"
#include "system/MetricsRegistry.h"

#include <chrono>

int main() {
    // Anchor the cold-start timer as early as possible. Captured here rather
    // than inside Engine so the measurement includes Engine construction +
    // window/GL/UI bring-up (i.e. everything between launch and the first
    // rendered frame).
    MetricsRegistry::SetProcessStartTime(std::chrono::steady_clock::now());

    // Engine owns all subsystems; Run() blocks until the window is closed.
    if (Engine engine; engine.Init()) {
        engine.Run();
    }
    return 0;
}
