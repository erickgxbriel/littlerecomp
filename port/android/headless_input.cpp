#include "core/input.h"

#include "app/app.h"

namespace input {

void Init() {}
uint16_t HeldOnAnyController() { return 0; }

Pad Poll(int port) {
    Pad p;
    p.connected = (port == 0);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
}

}  // namespace input
