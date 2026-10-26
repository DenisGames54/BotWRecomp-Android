#include "menu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "../settings.h"
#include "touch.h"
#include "window.h"

extern float g_render_scale;  // main.cpp: the scale this run renders at

namespace menu {
namespace {
std::mutex g_lock;
std::atomic<bool> g_open{false};
std::atomic<bool> g_drawer{false};  // Android back button: actions panel on the right
int g_cursor = 0;
int g_drawer_cursor = 0;
bool g_reset_armed = false;  // "Reset touch layout" needs a second press (guarded by g_lock)

enum RowId { kFps, kScale, kDisplay, kFilter, kGyro, kRumble, kLayout, kTouch, kCamSens, kEditTouch, kResetTouch, kShowFps, kVolume, kClose, kRows };
enum DrawerRow { kDrawerEdit, kDrawerReset, kDrawerQuit, kDrawerRows };
const float kScales[] = {0, 0.5f, 0.75f, 1, 1.5f, 2, 2.5f, 3};  // 0 = fit the display
const char* kDisplays[] = {"Fit 16:9", "Stretch", "Fill (crop)"};
const char* kFilters[] = {"Bilinear", "Bicubic", "Hermite", "Nearest"};

std::string scale_text(float s) {
    char b[64];
    if (s <= 0) return "Auto (fit the display)";
    snprintf(b, sizeof b, "%gx  (%dx%d)", s, (int)lroundf(1280 * s), (int)lroundf(720 * s));
    return b;
}

void step(Settings& s, int row, int dir) {
    auto wrap = [](int v, int n) { return (v % n + n) % n; };
    switch (row) {
        case kFps: s.fps = s.fps >= 60 ? 30 : 60; break;
        case kScale: {
            int i = 0;
            for (int k = 0; k < (int)std::size(kScales); k++)
                if (fabsf(kScales[k] - s.scale) < 0.01f) i = k;
            s.scale = kScales[wrap(i + dir, (int)std::size(kScales))];
            break;
        }
        case kDisplay: s.display = wrap(s.display + dir, 3); break;
        case kFilter: s.filter = wrap(s.filter + dir, 4); break;
        case kGyro: s.gyro = !s.gyro; break;
        case kRumble: s.rumble = !s.rumble; break;
        case kLayout: s.layout = !s.layout; break;
        case kTouch: s.touch = wrap(s.touch + dir, 3), s.touch_hidden = false; break;
        case kCamSens: s.cam_sens = std::clamp(lroundf((s.cam_sens + dir * 0.1f) * 10) / 10.0f, 0.3f, 3.0f); break;
        case kShowFps: s.show_fps = !s.show_fps; break;
        case kVolume: s.volume = std::clamp(s.volume + dir * 10, 0, 100); break;
    }
}

bool take_reset_arm() {  // first press arms, the second one resets (a custom layout is not lost by a mis-tap)
    std::lock_guard<std::mutex> l(g_lock);
    const bool go = g_reset_armed;
    g_reset_armed = !g_reset_armed;
    return go;
}

void drawer_action(int row) {
    switch (row) {
        case kDrawerEdit:
            close();
            touch::set_edit(true);
            break;
        case kDrawerReset:
            if (take_reset_arm()) {
                touch::reset_layout();
                close();  // show the default layout right away
            }
            break;
        case kDrawerQuit: window::quit(); break;
    }
}
}  // namespace

std::atomic<float> g_px{-1}, g_py{-1};
std::atomic<bool> g_pdown{false};
void pointer(float x, float y, bool down) {
    g_px = x;
    g_py = y;
    g_pdown = down;
}
void pointer_state(float* x, float* y, bool* down) {
    *x = g_px;
    *y = g_py;
    *down = g_pdown;
}

std::atomic<float> g_safe[4] = {0, 0, 0, 0};
void set_safe_area(float l, float t, float r, float b) {
    g_safe[0] = l, g_safe[1] = t, g_safe[2] = r, g_safe[3] = b;
}
void safe_area(float* l, float* t, float* r, float* b) {
    *l = g_safe[0], *t = g_safe[1], *r = g_safe[2], *b = g_safe[3];
}

std::atomic<float> g_panel[4] = {0, 0, 0, 0}, g_xbtn[4] = {0, 0, 0, 0};
void set_rects(const float p[4], const float x[4]) {
    for (int i = 0; i < 4; i++) g_panel[i] = p[i], g_xbtn[i] = x[i];
}
void rects(float p[4], float x[4]) {
    for (int i = 0; i < 4; i++) p[i] = g_panel[i], x[i] = g_xbtn[i];
}
bool touch_close(float x, float y) {
    const float slop = 0.04f;  // the X is small for a thumb
    bool outside = x < g_panel[0] || y < g_panel[1] || x > g_panel[2] || y > g_panel[3];
    bool on_x = x > g_xbtn[0] - slop && y > g_xbtn[1] - slop && x < g_xbtn[2] + slop && y < g_xbtn[3] + slop;
    if (!is_open() || !(outside || on_x)) return false;
    close();
    return true;
}

bool is_open() { return g_open || g_drawer; }
bool drawer_open() { return g_drawer; }
void toggle() {
    if (g_drawer) {
        close();
        return;
    }
    g_open = !g_open;
}
void close() {
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_reset_armed = false;
    }
    g_open = false;
    g_drawer = false;
}
void open_drawer() {
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_drawer_cursor = 0;
        g_reset_armed = false;
    }
    g_open = false;
    g_drawer = true;
}

void move(int dy) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_drawer) g_drawer_cursor = (g_drawer_cursor + dy + kDrawerRows) % kDrawerRows;
    else g_cursor = (g_cursor + dy + kRows) % kRows;
}

void change(int dir) {
    int row;
    bool drawer;
    {
        std::lock_guard<std::mutex> l(g_lock);
        drawer = g_drawer;
        row = drawer ? g_drawer_cursor : g_cursor;
    }
    if (drawer) {
        drawer_action(row);
        return;
    }
    if (row == kClose) return;
    if (row == kEditTouch) {  // runtime mode, not a Settings value: close the menu so the controls are visible
        touch::set_edit(true);
        close();
        return;
    }
    if (row == kResetTouch) {
        if (take_reset_arm()) {
            touch::reset_layout();
            close();
        }
        return;
    }
    Settings s = settings::get();
    step(s, row, dir);
    settings::set(s);
}

void activate() {
    int row;
    bool drawer;
    {
        std::lock_guard<std::mutex> l(g_lock);
        drawer = g_drawer;
        row = drawer ? g_drawer_cursor : g_cursor;
    }
    if (drawer) {
        drawer_action(row);
        return;
    }
    if (row == kClose) close();
    else change(+1);
}

void click(int row, int dir) {
    if (g_drawer) {
        if (row < 0 || row >= kDrawerRows) return;
        {
            std::lock_guard<std::mutex> l(g_lock);
            g_drawer_cursor = row;
        }
        drawer_action(row);
        return;
    }
    if (row < 0 || row >= kRows) return;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_cursor = row;
    }
    if (dir == 0) activate();
    else change(dir);
}

View view() {
    View v;
    v.open = g_open || g_drawer;
    v.drawer = g_drawer;
    if (v.drawer) {
        bool armed;
        {
            std::lock_guard<std::mutex> l(g_lock);
            v.cursor = g_drawer_cursor;
            armed = g_reset_armed;
        }
        v.rows.resize(kDrawerRows);
        v.rows[kDrawerEdit] = {"Edit touch layout", "", true};
        v.rows[kDrawerReset] = {armed ? "Tap again to reset" : "Reset touch layout", "", true};
        v.rows[kDrawerQuit] = {"Quit game", "", true};
        return v;
    }
    bool armed;
    {
        std::lock_guard<std::mutex> l(g_lock);
        v.cursor = g_cursor;
        armed = g_reset_armed;
    }
    Settings s = settings::get();
    char b[64];
    v.rows.resize(kRows);
    v.rows[kFps] = {"Frame rate", s.fps >= 60 ? "60 fps" : "30 fps (original)"};
    v.rows[kScale] = {"Resolution", scale_text(s.scale)};
    v.rows[kDisplay] = {"Aspect", kDisplays[std::clamp(s.display, 0, 2)]};
    v.rows[kFilter] = {"Upscale filter", kFilters[std::clamp(s.filter, 0, 3)]};
    v.rows[kGyro] = {"Gyro aiming", s.gyro ? "On" : "Off"};
    v.rows[kRumble] = {"Rumble", s.rumble ? "On" : "Off"};
    v.rows[kLayout] = {"Buttons", s.layout ? "A = bottom (Cross / Xbox A)" : "A = right (Circle / Xbox B)"};
    v.rows[kTouch] = {"Touch controls", s.touch == 1 ? "On" : s.touch == 2 ? "Off" : "Auto (no controller)"};
    snprintf(b, sizeof b, "%.1fx", s.cam_sens);
    v.rows[kCamSens] = {"Camera sensitivity", b};
    v.rows[kEditTouch] = {"Edit touch layout", "", true};
    v.rows[kResetTouch] = {armed ? "Tap again to reset" : "Reset touch layout", "", true};
    v.rows[kShowFps] = {"FPS counter", s.show_fps ? "On" : "Off"};
    snprintf(b, sizeof b, "%d%%", s.volume);
    v.rows[kVolume] = {"Volume", b};
    v.rows[kClose] = {"Close", "", true};
    snprintf(b, sizeof b, "Rendering at %gx now; resolution changes apply after a restart.", g_render_scale);
    v.note = b;
    return v;
}
}  // namespace menu
