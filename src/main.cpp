#include "core/Engine.h"

int main() {
    if (Engine engine; engine.Init()) {
        engine.Run();
    }
    return 0;
}
