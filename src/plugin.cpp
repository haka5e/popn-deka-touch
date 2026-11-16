// popn_deka_touch: touch layer for the two large Pop-Kun buttons on the Sub Screen window.
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

namespace {

enum SpiceStatus : int { SPICE_SUCCESS = 0 };
enum SpiceLogLevel : int { SPICE_MISC = 0, SPICE_INFO = 1, SPICE_WARNING = 2, SPICE_FATAL = 3 };
using SpiceLog = int(__cdecl *)(SpiceLogLevel, const char *, const char *);
using SpiceSetButton = int(__cdecl *)(uint32_t, bool, float);
using SpiceToast = int(__cdecl *)(int, const char *);
using SpiceDestroy = void(__cdecl *)();
struct SpiceSdkV0 {
    uint32_t size;
    SpiceLog log;
    void *get_game_info;
    void *get_avs_info;
    void *get_button;
    SpiceSetButton set_button;
    void *get_analog;
    void *set_analog;
    void *get_light;
    void *set_light;
    void *set_touch;
    void *clear_touch;
    void *insert_card;
    void *set_keypad;
    SpiceToast add_toast;
};
using SpiceInit = int(__cdecl *)(uint32_t, SpiceDestroy, void *);

constexpr int kRed = 0;
constexpr int kBlue = 1;
constexpr int kToggle = 2;
constexpr int kNone = -1;
constexpr uint32_t kMousePointer = 0xffffffffu;
constexpr UINT kTabletQueryGestureStatus = 0x02CC;

// "x,y,width,height" from the ini, in pixels of a 1280x800 sub screen.
struct IniRect {
    bool set = false;
    float x = 0, y = 0, w = 0, h = 0;
};

struct PictureConfig {
    std::wstring image, pressed_image;
    IniRect rect, touch;
};

struct Config {
    char target_window[128] = "Sub Screen";
    int button_id[2] = {13, 14};
    bool start_enabled = true;
    bool require_focus = true;
    int toggle_key = VK_F9;
    int zone_width_percent = 36;
    int zone_height_percent = 62;
    int dome_size_percent = 31;
    int dome_margin_percent = 3;
    int dome_bottom_percent = 1;
    int view_angle = 45;
    int toggle_scale_percent = 100;
    int overall_alpha = 255;
    int dome_alpha = 235;
    bool show_zone = false;
    int off_toggle_alpha = 140;
    uint32_t dome_color[2] = {0xe4202a, 0x2a8fe0};
    PictureConfig picture[2];
    std::wstring toggle_image[2];   // [0] = off, [1] = on
    IniRect toggle_rect;
    bool has_custom = false;
};

struct PixelBuffer {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
};

struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

struct Layout {
    int width = 0;
    int height = 0;
    float zone_width = 0;
    float zone_top = 0;
    float corner = 0;
    float dome_radius = 0;
    float dome_x[2] = {0, 0};
    float dome_bottom = 0;
    float scale = 1;
    float toggle_left = 0, toggle_top = 0, toggle_right = 0, toggle_bottom = 0;
    bool custom_picture[2] = {false, false};   // [Red]/[Blue] Rect given
    Box picture[2];
    bool custom_touch[2] = {false, false};     // TouchRect or Rect given
    Box touch[2];
};

struct Contact {
    uint32_t id = 0;
    int zone = kNone;
    bool used = false;
};

HMODULE g_module = nullptr;
SpiceSdkV0 g_spice{};
Config g_config{};
HANDLE g_stop_event = nullptr;
HANDLE g_worker = nullptr;
std::atomic<bool> g_enabled{true};
Layout g_layout{};
std::array<Contact, 16> g_contacts{};
int g_hold[2] = {0, 0};
bool g_sent[2] = {false, false};
bool g_toggle_key_was_down = false;

void log_message(SpiceLogLevel level, const char *message) {
    if (g_spice.log) g_spice.log(level, "popn_deka_touch", message);
}

std::string module_directory() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    char *slash = std::strrchr(path, '\\');
    if (slash) *slash = '\0';
    return path;
}

void load_config() {
    const std::string ini = module_directory() + "\\popn_deka_touch.ini";
    auto read_int = [&](const char *name, int fallback) {
        return static_cast<int>(GetPrivateProfileIntA("Touch", name, fallback, ini.c_str()));
    };
    auto read_color = [&](const char *name, uint32_t fallback) {
        char value[32]{};
        char fallback_text[16]{};
        std::snprintf(fallback_text, sizeof(fallback_text), "%06X", fallback & 0xffffff);
        GetPrivateProfileStringA("Touch", name, fallback_text, value,
                                 static_cast<DWORD>(sizeof(value)), ini.c_str());
        const char *start = value;
        if (*start == '#') ++start;
        else if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) start += 2;
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(start, &end, 16);
        return end != start && *end == '\0' && parsed <= 0xffffff
            ? static_cast<uint32_t>(parsed) : fallback;
    };
    GetPrivateProfileStringA("Touch", "TargetWindow", "Sub Screen", g_config.target_window,
                             static_cast<DWORD>(sizeof(g_config.target_window)), ini.c_str());
    g_config.button_id[kRed] = std::clamp(read_int("RedButton", 13), 0, 255);
    g_config.button_id[kBlue] = std::clamp(read_int("BlueButton", 14), 0, 255);
    g_config.start_enabled = read_int("StartEnabled", 1) != 0;
    g_config.require_focus = read_int("RequireFocus", 1) != 0;
    g_config.toggle_key = std::clamp(read_int("ToggleKey", VK_F9), 0, 255);
    g_config.zone_width_percent = std::clamp(read_int("ZoneWidthPercent", 36), 10, 50);
    g_config.zone_height_percent = std::clamp(read_int("ZoneHeightPercent", 62), 10, 100);
    g_config.dome_size_percent = std::clamp(read_int("DomeSizePercent", 31), 8, 50);
    g_config.dome_margin_percent = std::clamp(read_int("DomeMarginPercent", 3), 0, 30);
    g_config.dome_bottom_percent = std::clamp(read_int("DomeBottomPercent", 1), 0, 40);
    g_config.view_angle = std::clamp(read_int("ViewAngle", 45), 15, 75);
    g_config.toggle_scale_percent = std::clamp(read_int("ToggleScalePercent", 100), 50, 300);
    g_config.overall_alpha = std::clamp(read_int("OverallAlpha", 255), 30, 255);
    g_config.dome_alpha = std::clamp(read_int("DomeAlpha", 235), 30, 255);
    g_config.show_zone = read_int("ShowZone", 0) != 0;
    g_config.off_toggle_alpha = std::clamp(read_int("OffToggleAlpha", 140), 30, 255);
    g_config.dome_color[kRed] = read_color("RedColor", 0xe4202a);
    g_config.dome_color[kBlue] = read_color("BlueColor", 0x2a8fe0);
    wchar_t module_path[MAX_PATH]{};
    GetModuleFileNameW(g_module, module_path, MAX_PATH);
    std::wstring directory = module_path;
    directory.erase(directory.find_last_of(L'\\') + 1);
    const std::wstring wide_ini = directory + L"popn_deka_touch.ini";
    auto read_text = [&](const wchar_t *section, const wchar_t *name) {
        wchar_t value[512]{};
        GetPrivateProfileStringW(section, name, L"", value, 512, wide_ini.c_str());
        return std::wstring(value);
    };
    auto read_path = [&](const wchar_t *section, const wchar_t *name) {
        std::wstring path = read_text(section, name);
        if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
            path = path.substr(1, path.size() - 2);
        }
        const bool absolute = path.size() > 1 && (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\'));
        return path.empty() || absolute ? path : directory + path;
    };
    auto read_rect = [&](const wchar_t *section, const wchar_t *name) {
        IniRect rect;
        const std::wstring text = read_text(section, name);
        rect.set = swscanf_s(text.c_str(), L"%f ,%f ,%f ,%f", &rect.x, &rect.y, &rect.w, &rect.h) == 4 &&
            rect.w >= 1.0f && rect.h >= 1.0f;
        return rect;
    };
    const wchar_t *sections[2] = {L"Red", L"Blue"};
    g_config.has_custom = false;
    for (int side = 0; side < 2; ++side) {
        PictureConfig &picture = g_config.picture[side];
        picture.image = read_path(sections[side], L"Image");
        picture.pressed_image = read_path(sections[side], L"PressedImage");
        picture.rect = read_rect(sections[side], L"Rect");
        picture.touch = read_rect(sections[side], L"TouchRect");
        g_config.has_custom = g_config.has_custom || !picture.image.empty() || picture.rect.set;
    }
    g_config.toggle_image[1] = read_path(L"Toggle", L"OnImage");
    g_config.toggle_image[0] = read_path(L"Toggle", L"OffImage");
    g_config.toggle_rect = read_rect(L"Toggle", L"Rect");
    g_config.has_custom = g_config.has_custom || !g_config.toggle_image[0].empty() ||
        !g_config.toggle_image[1].empty();
    g_enabled.store(g_config.start_enabled);
}

Layout compute_layout(int width, int height) {
    Layout l;
    l.width = width;
    l.height = height;
    l.scale = height / 800.0f;
    l.zone_width = width * g_config.zone_width_percent / 100.0f;
    l.zone_top = height * (1.0f - g_config.zone_height_percent / 100.0f);
    l.corner = 44.0f * l.scale;
    l.dome_radius = width * g_config.dome_size_percent / 200.0f;
    const float margin = width * g_config.dome_margin_percent / 100.0f;
    l.dome_x[kRed] = margin + l.dome_radius;
    l.dome_x[kBlue] = width - margin - l.dome_radius;
    l.dome_bottom = height - height * g_config.dome_bottom_percent / 100.0f;
    const float ts = l.scale * g_config.toggle_scale_percent / 100.0f;
    l.toggle_right = width - 14.0f * ts;
    l.toggle_top = 14.0f * ts;
    l.toggle_left = l.toggle_right - 196.0f * ts;
    l.toggle_bottom = l.toggle_top + 56.0f * ts;
    const float sx = width / 1280.0f, sy = height / 800.0f;
    auto to_box = [&](const IniRect &rect) {
        return Box{rect.x * sx, rect.y * sy, (rect.x + rect.w) * sx, (rect.y + rect.h) * sy};
    };
    if (g_config.toggle_rect.set) {
        const Box box = to_box(g_config.toggle_rect);
        l.toggle_left = box.x0;
        l.toggle_top = box.y0;
        l.toggle_right = box.x1;
        l.toggle_bottom = box.y1;
    }
    for (int side = 0; side < 2; ++side) {
        const PictureConfig &picture = g_config.picture[side];
        if (picture.rect.set) {
            l.custom_picture[side] = true;
            l.picture[side] = to_box(picture.rect);
        }
        if (picture.touch.set || picture.rect.set) {
            l.custom_touch[side] = true;
            l.touch[side] = to_box(picture.touch.set ? picture.touch : picture.rect);
        }
    }
    return l;
}

int hit_test(const Layout &l, float x, float y) {
    const float pad = 8.0f * l.scale;
    if (x >= l.toggle_left - pad && x <= l.toggle_right + pad &&
        y >= l.toggle_top - pad && y <= l.toggle_bottom + pad) return kToggle;
    if (!g_enabled.load(std::memory_order_relaxed)) return kNone;
    for (int side = 0; side < 2; ++side) {
        if (l.custom_touch[side]) {
            const Box &box = l.touch[side];
            if (x >= box.x0 && x < box.x1 && y >= box.y0 && y < box.y1) return side;
            continue;
        }
        const float edge_distance = side == kRed ? x : l.width - x;
        if (edge_distance <= l.zone_width && y >= l.zone_top) return side;
        if (std::fabs(x - l.dome_x[side]) <= l.dome_radius &&
            y >= l.dome_bottom - l.dome_radius * 1.9f) return side;
    }
    return kNone;
}

void apply_button(int side) {
    const bool pressed = g_hold[side] > 0;
    if (pressed == g_sent[side]) return;
    g_sent[side] = pressed;
    if (g_spice.set_button) {
        g_spice.set_button(static_cast<uint32_t>(g_config.button_id[side]), pressed,
                           pressed ? 1.0f : 0.0f);
    }
}

void release_all() {
    for (Contact &contact : g_contacts) contact = {};
    g_hold[kRed] = g_hold[kBlue] = 0;
    apply_button(kRed);
    apply_button(kBlue);
}

void set_enabled(bool enabled) {
    if (g_enabled.exchange(enabled) == enabled) return;
    if (!enabled) release_all();
}

void contact_down(uint32_t id, float x, float y) {
    const int zone = hit_test(g_layout, x, y);
    if (zone == kToggle) {
        set_enabled(!g_enabled.load());
        return;
    }
    if (zone == kNone) return;
    for (Contact &contact : g_contacts) {
        if (contact.used && contact.id == id) return;
    }
    for (Contact &contact : g_contacts) {
        if (contact.used) continue;
        contact = {id, zone, true};
        ++g_hold[zone];
        apply_button(zone);
        return;
    }
}

void contact_up(uint32_t id) {
    for (Contact &contact : g_contacts) {
        if (!contact.used || contact.id != id) continue;
        const int zone = contact.zone;
        contact = {};
        if (g_hold[zone] > 0) --g_hold[zone];
        apply_button(zone);
        return;
    }
}

bool mouse_message_from_touch() {
    return (static_cast<ULONG_PTR>(GetMessageExtraInfo()) & 0xffffff00u) == 0xff515700u;
}

LRESULT CALLBACK overlay_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_POINTERDOWN: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            contact_down(GET_POINTERID_WPARAM(wparam), static_cast<float>(point.x),
                         static_cast<float>(point.y));
            return 0;
        }
        case WM_POINTERUP:
        case WM_POINTERLEAVE:
        case WM_POINTERCAPTURECHANGED:
            contact_up(GET_POINTERID_WPARAM(wparam));
            return 0;
        case WM_POINTERUPDATE:
        case WM_POINTERENTER:
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            if (!mouse_message_from_touch()) {
                SetCapture(hwnd);
                contact_down(kMousePointer, static_cast<float>(GET_X_LPARAM(lparam)),
                             static_cast<float>(GET_Y_LPARAM(lparam)));
            }
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) ReleaseCapture();
            contact_up(kMousePointer);
            return 0;
        case WM_CAPTURECHANGED:
            contact_up(kMousePointer);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        default:
            if (message == kTabletQueryGestureStatus) return 0x00000001 | 0x00000008 | 0x00000010 | 0x00010000;
            return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

uint32_t argb(int alpha, uint32_t rgb) {
    const int scaled = (std::clamp(alpha, 0, 255) * g_config.overall_alpha + 127) / 255;
    return (static_cast<uint32_t>(scaled) << 24) | (rgb & 0xffffff);
}

void blend_pixel(uint32_t &destination, uint32_t rgb, float alpha) {
    const uint32_t sa = static_cast<uint32_t>(alpha + 0.5f);
    if (!sa) return;
    const uint32_t inv = 255 - sa;
    const uint32_t da = destination >> 24;
    const uint32_t dr = (destination >> 16) & 0xff;
    const uint32_t dg = (destination >> 8) & 0xff;
    const uint32_t db = destination & 0xff;
    const uint32_t oa = sa + da * inv / 255;
    const uint32_t or_ = ((rgb >> 16) & 0xff) * sa / 255 + dr * inv / 255;
    const uint32_t og = ((rgb >> 8) & 0xff) * sa / 255 + dg * inv / 255;
    const uint32_t ob = (rgb & 0xff) * sa / 255 + db * inv / 255;
    destination = (oa << 24) | (or_ << 16) | (og << 8) | ob;
}

// Rounded box with antialiased edge. stroke > 0 draws only an outline of that thickness.
void add_round_box(PixelBuffer &buffer, float x0, float y0, float x1, float y1, float radius,
                   uint32_t color, float stroke = 0.0f) {
    const float alpha = static_cast<float>(color >> 24);
    if (alpha <= 0.0f) return;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
    radius = std::min(radius, std::min(hx, hy));
    const int left = std::clamp(static_cast<int>(std::floor(x0)) - 1, 0, buffer.width);
    const int top = std::clamp(static_cast<int>(std::floor(y0)) - 1, 0, buffer.height);
    const int right = std::clamp(static_cast<int>(std::ceil(x1)) + 1, 0, buffer.width);
    const int bottom = std::clamp(static_cast<int>(std::ceil(y1)) + 1, 0, buffer.height);
    for (int y = top; y < bottom; ++y) {
        uint32_t *row = buffer.pixels.data() + static_cast<size_t>(y) * buffer.width;
        const float qy = std::fabs(y + 0.5f - cy) - (hy - radius);
        for (int x = left; x < right; ++x) {
            const float qx = std::fabs(x + 0.5f - cx) - (hx - radius);
            const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
            float distance = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
            if (stroke > 0.0f) distance = std::fabs(distance + stroke * 0.5f) - stroke * 0.5f;
            const float coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
            if (coverage > 0.0f) blend_pixel(row[x], color, alpha * coverage);
        }
    }
}

void add_circle(PixelBuffer &buffer, float cx, float cy, float radius, uint32_t color,
                float stroke = 0.0f) {
    add_round_box(buffer, cx - radius, cy - radius, cx + radius, cy + radius, radius, color, stroke);
}

// Fills pixels where distance(x, y) < 0, antialiased over one pixel.
template <class Distance>
void add_shape(PixelBuffer &buffer, float x0, float y0, float x1, float y1, uint32_t color,
               Distance distance) {
    const float alpha = static_cast<float>(color >> 24);
    if (alpha <= 0.0f) return;
    const int left = std::clamp(static_cast<int>(std::floor(x0)) - 1, 0, buffer.width);
    const int top = std::clamp(static_cast<int>(std::floor(y0)) - 1, 0, buffer.height);
    const int right = std::clamp(static_cast<int>(std::ceil(x1)) + 1, 0, buffer.width);
    const int bottom = std::clamp(static_cast<int>(std::ceil(y1)) + 1, 0, buffer.height);
    for (int y = top; y < bottom; ++y) {
        uint32_t *row = buffer.pixels.data() + static_cast<size_t>(y) * buffer.width;
        for (int x = left; x < right; ++x) {
            const float coverage = std::clamp(0.5f - distance(x + 0.5f, y + 0.5f), 0.0f, 1.0f);
            if (coverage > 0.0f) blend_pixel(row[x], color, alpha * coverage);
        }
    }
}

float ellipse_distance(float x, float y, float cx, float cy, float rx, float ry) {
    const float nx = (x - cx) / rx, ny = (y - cy) / ry;
    return (std::sqrt(nx * nx + ny * ny) - 1.0f) * std::min(rx, ry);
}

void add_ellipse(PixelBuffer &buffer, float cx, float cy, float rx, float ry, uint32_t color) {
    add_shape(buffer, cx - rx, cy - ry, cx + rx, cy + ry, color,
              [=](float x, float y) { return ellipse_distance(x, y, cx, cy, rx, ry); });
}

void add_line(PixelBuffer &buffer, float ax, float ay, float bx, float by, float thickness,
              uint32_t color) {
    const float half = thickness * 0.5f;
    const float vx = bx - ax, vy = by - ay;
    const float length_squared = std::max(vx * vx + vy * vy, 0.0001f);
    add_shape(buffer, std::min(ax, bx) - half, std::min(ay, by) - half,
              std::max(ax, bx) + half, std::max(ay, by) + half, color, [=](float x, float y) {
        const float t = std::clamp(((x - ax) * vx + (y - ay) * vy) / length_squared, 0.0f, 1.0f);
        const float dx = x - (ax + vx * t), dy = y - (ay + vy * t);
        return std::sqrt(dx * dx + dy * dy) - half;
    });
}

uint32_t mix_rgb(uint32_t from, uint32_t to, float amount) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const float a = static_cast<float>((from >> shift) & 0xff);
        const float b = static_cast<float>((to >> shift) & 0xff);
        out |= static_cast<uint32_t>(a + (b - a) * amount + 0.5f) << shift;
    }
    return out;
}

// Large Pop-Kun button as a solid of revolution seen from above at ViewAngle degrees.
// Heights and radii are in units of the picture width.
constexpr float kPlateTop = 0.03f;
constexpr float kShellHeight = 0.14f;
constexpr float kLipHeight = 0.04f;
constexpr float kDomeHeight = 0.30f;
constexpr int kMaterialPlate = 0;
constexpr int kMaterialBody = 1;
constexpr int kMaterialBand = 2;

struct ProfilePoint {
    float radius;
    int material;
};

ProfilePoint profile(float h, float band_height) {
    if (h < 0.0f) return {0.0f, kMaterialPlate};
    if (h < kPlateTop) {
        const float t = h / kPlateTop;
        return {0.5f - 0.012f * t * t, kMaterialPlate};
    }
    const float shell_top = kPlateTop + kShellHeight;
    if (h < shell_top) {
        const float t = (h - kPlateTop) / kShellHeight;
        if (t < 0.7f) return {0.335f + 0.11f * std::sin(t / 0.7f * 1.5708f), kMaterialBody};
        const float lip = (t - 0.7f) / 0.3f;
        return {0.445f - 0.022f * lip * lip, kMaterialBody};
    }
    const float band_top = shell_top + band_height;
    if (h < band_top) return {0.402f, kMaterialBand};
    const float u = h - band_top;
    if (u < kLipHeight) return {0.425f + 0.025f * std::sin(u / kLipHeight * 1.5708f), kMaterialBody};
    const float d = (u - kLipHeight) / kDomeHeight;
    if (d >= 1.0f) return {0.0f, kMaterialBody};
    return {0.45f * std::sqrt(1.0f - d * d), kMaterialBody};
}

struct Sprite {
    PixelBuffer image;      // premultiplied, fully opaque where covered
    float ground_x = 0;     // centre of the base plate inside the image
    float ground_y = 0;
    int left = 0;           // where it is drawn on the sub screen
    int top = 0;
};

Sprite g_sprites[2][2];
Sprite g_toggle[2];   // optional switch pictures: [0] = off, [1] = on

uint32_t opaque(uint32_t rgb) { return 0xff000000u | (rgb & 0xffffff); }

void add_eyes(PixelBuffer &buffer, float cx, float ground_y, float w, float band_height,
              float sin_e, float cos_e, int side, uint32_t body) {
    const float eye_height = kPlateTop + kShellHeight + band_height + kLipHeight + kDomeHeight * 0.40f;
    const float eye_radius = 0.45f * std::sqrt(1.0f - 0.40f * 0.40f);
    const float rx = 0.058f * w, ry = 0.066f * w;
    const float stroke = std::max(2.0f, 0.022f * w);
    const uint32_t ink = opaque(0x1c1a1a);
    for (int eye = 0; eye < 2; ++eye) {
        const float angle = eye == 0 ? -0.27f : 0.33f;
        const float ex = cx + eye_radius * std::sin(angle) * w;
        const float ey = ground_y - eye_height * cos_e * w + eye_radius * std::cos(angle) * sin_e * w;
        add_ellipse(buffer, ex, ey, rx + 0.009f * w, ry + 0.009f * w,
                    opaque(mix_rgb(body, 0x000000, 0.35f)));
        add_ellipse(buffer, ex, ey, rx, ry, opaque(0xfbf8f2));
        if (side == kBlue) {
            const float ar = 0.040f * w, ay = ey + 0.024f * w;
            add_shape(buffer, ex - ar - stroke, ay - ar - stroke, ex + ar + stroke, ay + stroke, ink,
                      [=](float x, float y) {
                const float ring = std::fabs(std::sqrt((x - ex) * (x - ex) + (y - ay) * (y - ay)) - ar);
                return std::max(ring - stroke * 0.5f, y - ay);
            });
        } else if (eye == 0) {
            add_ellipse(buffer, ex + 0.010f * w, ey + 0.008f * w, 0.040f * w, 0.045f * w, ink);
            add_ellipse(buffer, ex - 0.006f * w, ey - 0.014f * w, 0.012f * w, 0.012f * w,
                        opaque(0xffffff));
        } else {
            add_line(buffer, ex + 0.036f * w, ey - 0.030f * w, ex - 0.030f * w, ey + 0.004f * w,
                     stroke, ink);
            add_line(buffer, ex - 0.030f * w, ey + 0.004f * w, ex + 0.040f * w, ey + 0.018f * w,
                     stroke, ink);
        }
    }
}

void render_sprite(Sprite &sprite, float w, int side, bool pressed) {
    constexpr int kSuper = 3;
    const float elevation = g_config.view_angle * 3.14159265f / 180.0f;
    const float sin_e = std::sin(elevation), cos_e = std::cos(elevation);
    const float band_height = pressed ? 0.03f : 0.10f;
    const float top_height = kPlateTop + kShellHeight + band_height + kLipHeight + kDomeHeight;
    const uint32_t body = pressed ? mix_rgb(g_config.dome_color[side], 0xffffff, 0.14f)
                                  : g_config.dome_color[side];
    const uint32_t material_color[3] = {0x26262a, body, 0xf4f0e6};
    const float material_gloss[3] = {0.45f, 0.60f, 0.12f};

    float top_extent = 0.0f;
    for (int i = 0; i <= 200; ++i) {
        const float h = top_height * i / 200.0f;
        top_extent = std::max(top_extent, h * cos_e + profile(h, band_height).radius * sin_e);
    }
    const float pad = 3.0f;
    const int width = static_cast<int>(std::ceil(w + pad * 2));
    const int height = static_cast<int>(std::ceil((top_extent + 0.5f * sin_e) * w + pad * 2));
    sprite.ground_x = width * 0.5f;
    sprite.ground_y = pad + top_extent * w;

    const int hi_width = width * kSuper, hi_height = height * kSuper;
    std::vector<uint32_t> hi(static_cast<size_t>(hi_width) * hi_height, 0);
    const float W = w * kSuper;
    const float gx = sprite.ground_x * kSuper, gy = sprite.ground_y * kSuper;

    // Light from the upper left, viewer in front and above. x right, y toward viewer, z up.
    const float light[3] = {-0.47f, 0.33f, 0.82f};
    float half_vector[3] = {light[0], light[1] + cos_e, light[2] + sin_e};
    const float half_length = std::sqrt(half_vector[0] * half_vector[0] +
        half_vector[1] * half_vector[1] + half_vector[2] * half_vector[2]);
    for (float &component : half_vector) component /= half_length;

    const float step = 0.7f / (W * cos_e);
    const int slices = static_cast<int>(top_height / step);
    for (int i = 0; i <= slices; ++i) {
        const float h = i * step;
        const ProfilePoint point = profile(h, band_height);
        if (point.radius <= 0.0f) continue;
        const ProfilePoint next = profile(h + step, band_height);
        const float R = point.radius * W, Rn = next.radius * W;
        const float cy = gy - h * W * cos_e, cyn = gy - (h + step) * W * cos_e;
        const float slope = std::clamp((profile(h + 0.004f, band_height).radius -
            profile(h - 0.004f, band_height).radius) / 0.008f, -6.0f, 6.0f);
        const float slope_length = std::sqrt(1.0f + slope * slope);
        const float side_weight = 1.0f / slope_length, up_weight = -slope / slope_length;
        const uint32_t base = material_color[point.material];
        const float gloss = material_gloss[point.material];

        const int row_begin = std::max(0, static_cast<int>(std::floor(cy - R * sin_e)));
        const int row_end = std::min(hi_height - 1, static_cast<int>(std::ceil(cy + R * sin_e)));
        for (int y = row_begin; y <= row_end; ++y) {
            const float v = (y + 0.5f - cy) / (R * sin_e);
            if (std::fabs(v) >= 1.0f) continue;
            const float half = R * std::sqrt(1.0f - v * v);
            float inner = -1.0f;
            if (Rn > 0.0f) {
                const float vn = (y + 0.5f - cyn) / (Rn * sin_e);
                if (std::fabs(vn) < 1.0f) inner = Rn * std::sqrt(1.0f - vn * vn) - 1.5f;
            }
            uint32_t *row = hi.data() + static_cast<size_t>(y) * hi_width;
            const int x_begin = std::max(0, static_cast<int>(std::floor(gx - half)));
            const int x_end = std::min(hi_width - 1, static_cast<int>(std::ceil(gx + half)) - 1);
            for (int x = x_begin; x <= x_end; ++x) {
                const float dx = x + 0.5f - gx;
                if (inner > 0.0f && std::fabs(dx) < inner) {
                    x = static_cast<int>(gx + inner - 0.5f);
                    continue;
                }
                if (std::fabs(dx) > half) continue;
                const float u = dx / R;
                const float rr = std::sqrt(u * u + v * v);
                const float blend = std::clamp((rr - 0.80f) / 0.18f, 0.0f, 1.0f);
                const float rim = blend * blend * (3.0f - 2.0f * blend);
                float nx = 0.0f, ny = 0.0f, nz = 1.0f;
                if (rr > 0.001f) {
                    nx = u / rr * side_weight * rim;
                    ny = v / rr * side_weight * rim;
                    nz = up_weight * rim + (1.0f - rim);
                    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (length > 0.001f) { nx /= length; ny /= length; nz /= length; }
                }
                const float diffuse = std::max(nx * light[0] + ny * light[1] + nz * light[2], 0.0f);
                const float sky = 0.5f + 0.5f * nz;
                const float intensity = std::min(0.50f + 0.14f * sky + 0.46f * diffuse, 1.08f);
                const float shine = std::max(nx * half_vector[0] + ny * half_vector[1] +
                    nz * half_vector[2], 0.0f);
                const float specular = gloss * std::pow(shine, 60.0f);
                uint32_t rgb = 0;
                for (int shift = 0; shift <= 16; shift += 8) {
                    float channel = static_cast<float>((base >> shift) & 0xff) * intensity;
                    channel = std::min(channel, 255.0f);
                    channel += (255.0f - channel) * specular;
                    rgb |= static_cast<uint32_t>(std::min(channel, 255.0f)) << shift;
                }
                row[x] = 0xff000000u | rgb;
            }
        }
    }

    sprite.image.width = width;
    sprite.image.height = height;
    sprite.image.pixels.assign(static_cast<size_t>(width) * height, 0);
    const uint32_t samples = kSuper * kSuper;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t r = 0, g = 0, b = 0, covered = 0;
            for (int sy = 0; sy < kSuper; ++sy) {
                const uint32_t *row = hi.data() + static_cast<size_t>(y * kSuper + sy) * hi_width;
                for (int sx = 0; sx < kSuper; ++sx) {
                    const uint32_t pixel = row[x * kSuper + sx];
                    if (!(pixel >> 24)) continue;
                    ++covered;
                    r += (pixel >> 16) & 0xff;
                    g += (pixel >> 8) & 0xff;
                    b += pixel & 0xff;
                }
            }
            if (!covered) continue;
            sprite.image.pixels[static_cast<size_t>(y) * width + x] =
                ((255 * covered / samples) << 24) | ((r / samples) << 16) |
                ((g / samples) << 8) | (b / samples);
        }
    }
    add_eyes(sprite.image, sprite.ground_x, sprite.ground_y, w, band_height, sin_e, cos_e, side, body);
}

// Loads a picture file and stretches it to width x height (height <= 0 keeps the aspect ratio).
bool load_image(const std::wstring &path, float width, float height, PixelBuffer &out) {
    Gdiplus::Bitmap source(path.c_str());
    if (source.GetLastStatus() != Gdiplus::Ok || !source.GetWidth() || !source.GetHeight()) return false;
    const int source_width = static_cast<int>(source.GetWidth());
    const int source_height = static_cast<int>(source.GetHeight());
    const int target_width = std::clamp(static_cast<int>(std::lround(width)), 1, 8192);
    const int target_height = std::clamp(static_cast<int>(std::lround(
        height > 0.0f ? height : width * source_height / source_width)), 1, 8192);
    Gdiplus::Bitmap target(target_width, target_height, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics graphics(&target);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::ImageAttributes attributes;
        attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        if (graphics.DrawImage(&source, Gdiplus::Rect(0, 0, target_width, target_height), 0, 0,
                               source_width, source_height, Gdiplus::UnitPixel, &attributes) != Gdiplus::Ok) {
            return false;
        }
    }
    Gdiplus::BitmapData data{};
    Gdiplus::Rect area(0, 0, target_width, target_height);
    if (target.LockBits(&area, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok) {
        return false;
    }
    out.width = target_width;
    out.height = target_height;
    out.pixels.resize(static_cast<size_t>(target_width) * target_height);
    for (int y = 0; y < target_height; ++y) {
        std::memcpy(out.pixels.data() + static_cast<size_t>(y) * target_width,
                    static_cast<const uint8_t *>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride,
                    static_cast<size_t>(target_width) * sizeof(uint32_t));
    }
    target.UnlockBits(&data);
    return true;
}

void log_missing_image(const std::wstring &path) {
    char narrow[600]{};
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    const std::string message = std::string("could not load image, using the built-in picture: ") + narrow;
    log_message(SPICE_WARNING, message.c_str());
}

// Pressed look for a picture that has no PressedImage: the same picture, lighter.
void brighten(PixelBuffer &image, float amount) {
    for (uint32_t &pixel : image.pixels) {
        const uint32_t alpha = pixel >> 24;
        if (!alpha) continue;
        uint32_t out = alpha << 24;
        for (int shift = 0; shift <= 16; shift += 8) {
            const float channel = static_cast<float>((pixel >> shift) & 0xff);
            out |= static_cast<uint32_t>(channel + (alpha - channel) * amount) << shift;
        }
        pixel = out;
    }
}

// Builds the two pictures (normal, pressed) per button and the optional switch pictures.
void prepare_pictures(const Layout &l) {
    for (int side = 0; side < 2; ++side) {
        const PictureConfig &config = g_config.picture[side];
        const bool custom = l.custom_picture[side];
        const Box &box = l.picture[side];
        const float width = custom ? box.x1 - box.x0 : l.dome_radius * 2.0f;
        const float centre = custom ? (box.x0 + box.x1) * 0.5f : l.dome_x[side];
        const float bottom = custom ? box.y1 : l.dome_bottom;
        Sprite &normal = g_sprites[side][0];
        Sprite &down = g_sprites[side][1];
        bool loaded = false;
        if (!config.image.empty()) {
            loaded = load_image(config.image, width, custom ? box.y1 - box.y0 : 0.0f, normal.image);
            if (!loaded) log_missing_image(config.image);
        }
        if (loaded) {
            normal.left = static_cast<int>(std::lround(centre - normal.image.width * 0.5f));
            normal.top = static_cast<int>(std::lround(bottom)) - normal.image.height;
            const bool has_pressed = !config.pressed_image.empty() &&
                load_image(config.pressed_image, static_cast<float>(normal.image.width),
                           static_cast<float>(normal.image.height), down.image);
            if (!config.pressed_image.empty() && !has_pressed) log_missing_image(config.pressed_image);
            if (!has_pressed) {
                down.image = normal.image;
                brighten(down.image, 0.22f);
            }
            down.left = normal.left;
            down.top = normal.top;
            continue;
        }
        for (int pressed = 0; pressed < 2; ++pressed) {
            Sprite &sprite = g_sprites[side][pressed];
            render_sprite(sprite, width, side, pressed != 0);
            sprite.left = static_cast<int>(std::lround(centre - sprite.ground_x));
            sprite.top = static_cast<int>(std::lround(bottom)) - sprite.image.height;
        }
    }
    for (int state = 0; state < 2; ++state) {
        Sprite &sprite = g_toggle[state];
        sprite.image = {};
        const std::wstring &path = g_config.toggle_image[state];
        if (path.empty()) continue;
        if (!load_image(path, l.toggle_right - l.toggle_left, l.toggle_bottom - l.toggle_top, sprite.image)) {
            sprite.image = {};
            log_missing_image(path);
            continue;
        }
        sprite.left = static_cast<int>(std::lround(l.toggle_left));
        sprite.top = static_cast<int>(std::lround(l.toggle_top));
    }
}

void blit_sprite(PixelBuffer &buffer, const Sprite &sprite, int alpha) {
    const int scale = (std::clamp(alpha, 0, 255) * g_config.overall_alpha + 127) / 255;
    const int left = sprite.left;
    const int top = sprite.top;
    for (int y = 0; y < sprite.image.height; ++y) {
        const int ty = top + y;
        if (ty < 0 || ty >= buffer.height) continue;
        const uint32_t *source = sprite.image.pixels.data() + static_cast<size_t>(y) * sprite.image.width;
        uint32_t *row = buffer.pixels.data() + static_cast<size_t>(ty) * buffer.width;
        for (int x = 0; x < sprite.image.width; ++x) {
            const int tx = left + x;
            if (tx < 0 || tx >= buffer.width || !(source[x] >> 24)) continue;
            const uint32_t sa = (source[x] >> 24) * scale / 255;
            const uint32_t inv = 255 - sa;
            const uint32_t d = row[tx];
            const uint32_t oa = sa + (d >> 24) * inv / 255;
            const uint32_t or_ = ((source[x] >> 16) & 0xff) * scale / 255 + ((d >> 16) & 0xff) * inv / 255;
            const uint32_t og = ((source[x] >> 8) & 0xff) * scale / 255 + ((d >> 8) & 0xff) * inv / 255;
            const uint32_t ob = (source[x] & 0xff) * scale / 255 + (d & 0xff) * inv / 255;
            row[tx] = (std::min(oa, 255u) << 24) | (std::min(or_, 255u) << 16) |
                (std::min(og, 255u) << 8) | std::min(ob, 255u);
        }
    }
}

const uint8_t *glyph(char ch) {
    static const uint8_t O[7]={14,17,17,17,17,17,14}, N[7]={17,25,21,19,17,17,17};
    static const uint8_t F[7]={31,16,16,30,16,16,16}, SPACE[7]={0,0,0,0,0,0,0};
    switch (ch) {
        case 'O': return O; case 'N': return N; case 'F': return F; default: return SPACE;
    }
}

float text_width(const char *s, float scale) {
    return static_cast<float>(std::strlen(s)) * 6.0f * scale - scale;
}

void add_text(PixelBuffer &buffer, float x, float y, const char *s, float scale, uint32_t c) {
    for (; *s; ++s, x += 6.0f * scale) {
        const uint8_t *rows = glyph(*s);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (!(rows[row] & (1u << (4 - col)))) continue;
                const float px = x + col * scale, py = y + row * scale;
                add_round_box(buffer, px, py, px + scale, py + scale, 0.0f, c);
            }
        }
    }
}

// Everything except the two pictures: invisible touch zones and the ON/OFF switch.
void build_base(PixelBuffer &buffer, const Layout &l, bool enabled) {
    buffer.width = l.width;
    buffer.height = l.height;
    buffer.pixels.assign(static_cast<size_t>(l.width) * l.height, 0);

    if (enabled) {
        for (int side = 0; side < 2; ++side) {
            // Touch areas are always painted with at least alpha 1: fully transparent pixels of
            // a layered window let touches fall through to the game, alpha 1 is not visible.
            const uint32_t invisible = 0x01000000u;
            const uint32_t fill = g_config.show_zone ? argb(26, g_config.dome_color[side]) | invisible
                                                     : invisible;
            const uint32_t outline = argb(150, g_config.dome_color[side]);
            const float outline_width = std::max(2.0f, 2.5f * l.scale);
            if (l.custom_touch[side]) {
                const Box &box = l.touch[side];
                add_round_box(buffer, box.x0, box.y0, box.x1, box.y1, 0.0f, fill);
                if (g_config.show_zone) {
                    add_round_box(buffer, box.x0, box.y0, box.x1, box.y1, 0.0f, outline, outline_width);
                }
                continue;
            }
            const float x0 = side == kRed ? -l.corner * 2 : l.width - l.zone_width;
            const float x1 = side == kRed ? l.zone_width : l.width + l.corner * 2;
            const float y1 = l.height + l.corner * 2;
            add_round_box(buffer, x0, l.zone_top, x1, y1, l.corner, fill);
            add_round_box(buffer, l.dome_x[side] - l.dome_radius, l.dome_bottom - l.dome_radius * 1.9f,
                          l.dome_x[side] + l.dome_radius, static_cast<float>(l.height), 0.0f, invisible);
            if (g_config.show_zone) {
                add_round_box(buffer, x0, l.zone_top, x1, y1, l.corner, outline, outline_width);
            }
        }
    }

    if (g_toggle[enabled ? 1 : 0].image.width) {
        add_round_box(buffer, l.toggle_left, l.toggle_top, l.toggle_right, l.toggle_bottom, 0.0f,
                      0x01000000u);
        blit_sprite(buffer, g_toggle[enabled ? 1 : 0], 255);
        return;
    }
    const float th = l.toggle_bottom - l.toggle_top;
    const int pill_alpha = enabled ? 235 : g_config.off_toggle_alpha;
    add_round_box(buffer, l.toggle_left, l.toggle_top, l.toggle_right, l.toggle_bottom, th * 0.5f,
                  argb(pill_alpha, 0x444441));
    add_round_box(buffer, l.toggle_left, l.toggle_top, l.toggle_right, l.toggle_bottom, th * 0.5f,
                  argb(pill_alpha, 0x888780), std::max(1.0f, th / 28.0f));
    const float track_h = th * 0.64f, track_w = track_h * 1.8f;
    const float track_right = l.toggle_right - (th - track_h) * 0.5f;
    const float track_top = l.toggle_top + (th - track_h) * 0.5f;
    add_round_box(buffer, track_right - track_w, track_top, track_right, track_top + track_h,
                  track_h * 0.5f, argb(pill_alpha, enabled ? 0x1d9e75 : 0x888780));
    const float knob_r = track_h * 0.38f;
    const float knob_x = enabled ? track_right - track_h * 0.5f : track_right - track_w + track_h * 0.5f;
    add_circle(buffer, knob_x, track_top + track_h * 0.5f, knob_r, argb(pill_alpha, 0xf1efe8));
    const char *state = enabled ? "ON" : "OFF";
    const float text_scale = std::max(2.0f, std::round(th / 16.0f));
    const float text_area = track_right - track_w - l.toggle_left;
    add_text(buffer, l.toggle_left + (text_area - text_width(state, text_scale)) * 0.5f,
             l.toggle_top + (th - 7.0f * text_scale) * 0.5f, state, text_scale,
             argb(pill_alpha, 0xf1efe8));
}

PixelBuffer g_base[2];

void compose_full(PixelBuffer &frame, const Layout &, bool enabled, const bool pressed[2]) {
    frame = g_base[enabled ? 1 : 0];
    if (!enabled) return;
    for (int side = 0; side < 2; ++side) {
        blit_sprite(frame, g_sprites[side][pressed[side] ? 1 : 0], g_config.dome_alpha);
    }
}

// Redraws only the rectangle one picture occupies and returns it.
RECT compose_side(PixelBuffer &frame, const Layout &, int side, bool pressed) {
    const Sprite &a = g_sprites[side][0], &b = g_sprites[side][1];
    RECT r{};
    r.left = std::clamp(std::min(a.left, b.left), 0, frame.width);
    r.top = std::clamp(std::min(a.top, b.top), 0, frame.height);
    r.right = std::clamp(std::max(a.left + a.image.width, b.left + b.image.width), 0, frame.width);
    r.bottom = std::clamp(std::max(a.top + a.image.height, b.top + b.image.height), 0, frame.height);
    const PixelBuffer &base = g_base[1];
    for (int y = r.top; y < r.bottom; ++y) {
        const size_t offset = static_cast<size_t>(y) * frame.width + r.left;
        std::memcpy(frame.pixels.data() + offset, base.pixels.data() + offset,
                    static_cast<size_t>(r.right - r.left) * sizeof(uint32_t));
    }
    blit_sprite(frame, g_sprites[side][pressed ? 1 : 0], g_config.dome_alpha);
    return r;
}

struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    uint32_t *bits = nullptr;
    int width = 0;
    int height = 0;
};

void free_surface(Surface &surface) {
    if (surface.dc && surface.previous) SelectObject(surface.dc, surface.previous);
    if (surface.bitmap) DeleteObject(surface.bitmap);
    if (surface.dc) DeleteDC(surface.dc);
    surface = {};
}

bool ensure_surface(Surface &surface, int width, int height) {
    if (surface.bits && surface.width == width && surface.height == height) return true;
    free_surface(surface);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    surface.dc = CreateCompatibleDC(nullptr);
    surface.bitmap = surface.dc ? CreateDIBSection(surface.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0)
                                : nullptr;
    if (!surface.bitmap || !bits) {
        free_surface(surface);
        return false;
    }
    surface.previous = SelectObject(surface.dc, surface.bitmap);
    surface.bits = static_cast<uint32_t *>(bits);
    surface.width = width;
    surface.height = height;
    return true;
}

// dirty == nullptr uploads the whole frame; otherwise only that rectangle changed.
bool present_overlay(HWND overlay, Surface &surface, const RECT &screen_rect,
                     const PixelBuffer &buffer, const RECT *dirty) {
    const bool fresh = !surface.bits || surface.width != buffer.width || surface.height != buffer.height;
    if (!ensure_surface(surface, buffer.width, buffer.height)) return false;
    RECT area{0, 0, buffer.width, buffer.height};
    if (dirty && !fresh) area = *dirty;
    for (int y = area.top; y < area.bottom; ++y) {
        const size_t offset = static_cast<size_t>(y) * buffer.width + area.left;
        std::memcpy(surface.bits + offset, buffer.pixels.data() + offset,
                    static_cast<size_t>(area.right - area.left) * sizeof(uint32_t));
    }
    POINT position{screen_rect.left, screen_rect.top};
    SIZE size{buffer.width, buffer.height};
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info{};
    info.cbSize = sizeof(info);
    info.pptDst = &position;
    info.psize = &size;
    info.hdcSrc = surface.dc;
    info.pptSrc = &source;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = dirty && !fresh ? &area : nullptr;
    return UpdateLayeredWindowIndirect(overlay, &info) != FALSE;
}

struct WindowSearch {
    HWND excluded = nullptr;
    HWND found = nullptr;
};

BOOL CALLBACK find_target_callback(HWND hwnd, LPARAM value) {
    auto &search = *reinterpret_cast<WindowSearch *>(value);
    if (hwnd == search.excluded || !IsWindowVisible(hwnd)) return TRUE;
    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id != GetCurrentProcessId()) return TRUE;
    char title[128]{};
    GetWindowTextA(hwnd, title, static_cast<int>(sizeof(title)));
    if (_stricmp(title, g_config.target_window) != 0) return TRUE;
    search.found = hwnd;
    return FALSE;
}

HWND find_target_window(HWND overlay) {
    WindowSearch search{overlay};
    EnumWindows(find_target_callback, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool target_window_rect(HWND target, RECT &screen_rect) {
    if (!target || !IsWindow(target) || !IsWindowVisible(target) || IsIconic(target)) return false;
    RECT client{};
    if (!GetClientRect(target, &client)) return false;
    POINT origin{client.left, client.top};
    if (!ClientToScreen(target, &origin)) return false;
    screen_rect = {origin.x, origin.y, origin.x + client.right - client.left,
                   origin.y + client.bottom - client.top};
    return screen_rect.right - screen_rect.left >= 320 && screen_rect.bottom - screen_rect.top >= 200;
}

bool process_has_focus() {
    if (!g_config.require_focus) return true;
    DWORD process_id = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process_id);
    return process_id == GetCurrentProcessId();
}

void poll_toggle_key() {
    if (!g_config.toggle_key) return;
    const bool key_down = (GetAsyncKeyState(g_config.toggle_key) & 0x8000) != 0;
    if (key_down && !g_toggle_key_was_down && process_has_focus()) set_enabled(!g_enabled.load());
    g_toggle_key_was_down = key_down;
}

void CALLBACK location_changed(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {}

DWORD WINAPI worker_main(void *) {
    load_config();
    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr);
    constexpr wchar_t kOverlayClass[] = L"PopnDekaTouchWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = overlay_window_proc;
    window_class.hInstance = g_module;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = kOverlayClass;
    const ATOM registered = RegisterClassExW(&window_class);
    HWND overlay = nullptr;
    if (registered || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        overlay = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            kOverlayClass, L"pop'n deka touch", WS_POPUP, 0, 0, 1, 1,
            nullptr, nullptr, g_module, nullptr);
    }
    if (!overlay) {
        log_message(SPICE_WARNING, "failed to create touch layer window");
        return 0;
    }
    for (int feedback = FEEDBACK_TOUCH_CONTACTVISUALIZATION; feedback <= FEEDBACK_TOUCH_RIGHTTAP;
         ++feedback) {
        BOOL off = FALSE;
        SetWindowFeedbackSetting(overlay, static_cast<FEEDBACK_TYPE>(feedback), 0, sizeof(off), &off);
    }
    log_message(SPICE_INFO, "touch layer ready (waiting for the sub screen window)");
    if (g_spice.add_toast) g_spice.add_toast(1, "Deka Pop-Kun touch layer ready (F9)");

    HWINEVENTHOOK move_hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_OBJECT_LOCATIONCHANGE, nullptr, location_changed, GetCurrentProcessId(), 0,
        WINEVENT_OUTOFCONTEXT);
    HWND target = nullptr;
    RECT last_rect{-1, -1, -1, -1};
    unsigned int scan_counter = 0;
    bool visible = false;
    bool attach_logged = false;
    bool drawn_enabled = false;
    bool drawn_pressed[2] = {false, false};
    PixelBuffer frame;
    Surface surface;
    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &g_stop_event, FALSE, 16, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        poll_toggle_key();

        if (!target || !IsWindow(target) || (++scan_counter % 60) == 0) {
            target = find_target_window(overlay);
        }
        RECT rect{};
        const bool should_show = target_window_rect(target, rect) && process_has_focus();
        if (!should_show) {
            if (visible) {
                release_all();
                ShowWindow(overlay, SW_HIDE);
                visible = false;
            }
            continue;
        }
        const bool rect_changed = std::memcmp(&rect, &last_rect, sizeof(RECT)) != 0;
        const bool size_changed = rect.right - rect.left != g_layout.width ||
            rect.bottom - rect.top != g_layout.height;
        if (size_changed) {
            const Layout layout = compute_layout(rect.right - rect.left, rect.bottom - rect.top);
            if (layout.dome_radius != g_layout.dome_radius || g_config.has_custom) {
                prepare_pictures(layout);
            }
            g_layout = layout;
            build_base(g_base[0], g_layout, false);
            build_base(g_base[1], g_layout, true);
        }
        const bool enabled = g_enabled.load(std::memory_order_relaxed);
        const bool pressed[2] = {g_sent[kRed], g_sent[kBlue]};
        const bool moved_only = rect_changed && !size_changed && visible &&
            enabled == drawn_enabled && last_rect.right - last_rect.left == g_layout.width &&
            last_rect.bottom - last_rect.top == g_layout.height;
        if (moved_only) {
            SetWindowPos(overlay, nullptr, rect.left, rect.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            last_rect = rect;
        } else if (rect_changed || !visible || enabled != drawn_enabled) {
            compose_full(frame, g_layout, enabled, pressed);
            if (present_overlay(overlay, surface, rect, frame, nullptr)) {
                last_rect = rect;
                drawn_enabled = enabled;
                drawn_pressed[kRed] = pressed[kRed];
                drawn_pressed[kBlue] = pressed[kBlue];
            }
        } else if (enabled) {
            for (int side = 0; side < 2; ++side) {
                if (pressed[side] == drawn_pressed[side]) continue;
                const RECT dirty = compose_side(frame, g_layout, side, pressed[side]);
                if (present_overlay(overlay, surface, rect, frame, &dirty)) {
                    drawn_pressed[side] = pressed[side];
                }
            }
        }
        if (!visible || (scan_counter % 60) == 0) {
            SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            visible = true;
            if (!attach_logged) {
                log_message(SPICE_INFO, "touch layer attached to the sub screen window");
                attach_logged = true;
            }
        }
    }
    if (move_hook) UnhookWinEvent(move_hook);
    free_surface(surface);
    release_all();
    DestroyWindow(overlay);
    if (registered) UnregisterClassW(kOverlayClass, g_module);
    if (gdiplus_token) Gdiplus::GdiplusShutdown(gdiplus_token);
    return 0;
}

void __cdecl destroy_plugin() {
    if (g_stop_event) SetEvent(g_stop_event);
    if (g_worker) {
        WaitForSingleObject(g_worker, 5000);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    if (g_stop_event) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
    log_message(SPICE_INFO, "plugin unloaded");
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl spice_sdk_entry_point(SpiceInit init) {
    g_spice = {};
    g_spice.size = sizeof(g_spice);
    if (!init || init(0, destroy_plugin, &g_spice) != SPICE_SUCCESS) return 0;
    log_message(SPICE_INFO, "plugin loaded");
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) return 0;
    g_worker = CreateThread(nullptr, 0, worker_main, nullptr, 0, nullptr);
    if (!g_worker) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        return 0;
    }
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
