// On-screen touch controls (iPhone / iPad, or a touchscreen Mac). Host side (window.mm) feeds SDL finger
// events and merges the virtual pad into the GamePad state; the overlay (cemu_shims/overlay_menu.cpp)
// calls draw() inside its ImGui pass on the GPU thread.
#pragma once
#include <SDL3/SDL_events.h>

#include <string>

#include "../input/input.h"

namespace touch {
void set_mode(int mode);              // 0 auto (shown while no controller is connected), 1 on, 2 off
void set_controller_connected(bool connected);
void set_hidden(bool hidden);          // collapsed: only the show/hide button is shown and live
bool active();                        // controls currently shown and live
// Layout editor: every control follows the finger until it is lifted; the green check in the middle
// saves and leaves. While editing the game receives no touch input.
void set_edit(bool on);
bool edit();
// Custom positions from settings.ini: "dx,dy;dx,dy;..." in design points, one pair per control in the
// order touch.cpp builds them (empty or missing pairs = default layout).
void set_layout(const std::string& spec);
void event(const SDL_Event& e);       // SDL_EVENT_FINGER_* (host main thread)
void apply(input::PadState& s);       // add virtual buttons / sticks (host main thread, every loop)
void draw();                          // GPU thread, inside the overlay's ImGui frame
}  // namespace touch
