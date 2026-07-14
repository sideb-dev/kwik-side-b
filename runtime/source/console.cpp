#include "gml_runtime.h"
#include "engine_internal.h"
#include "render.h"
#include "vcr_ttf_data.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace gml {

bool g_console_open = false;
double g_timescale = 1.0;

namespace {

constexpr int VK_BACKSPACE = 8;
constexpr int VK_ENTER = 13;
constexpr int VK_SHIFT = 16;
constexpr int VK_ESCAPE = 27;
constexpr int VK_SPACE = 32;
constexpr int VK_PAGE_UP = 33;
constexpr int VK_PAGE_DOWN = 34;
constexpr int VK_END = 35;
constexpr int VK_HOME = 36;
constexpr int VK_LEFT = 37;
constexpr int VK_UP = 38;
constexpr int VK_RIGHT = 39;
constexpr int VK_DOWN = 40;
constexpr int VK_DELETE = 46;
constexpr int VK_OEM_MINUS = 189;
constexpr int VK_OEM_PERIOD = 190;
constexpr int VK_OEM_TILDE = 192;

constexpr unsigned int kColBorder = 0x8C8C8C;
constexpr unsigned int kColInput = 0xFFFFFF;
constexpr unsigned int kColLog = 0xE0E0E0;
constexpr unsigned int kColError = 0x3030FF;

enum ConsoleState { S_CLOSED = 0, S_TYPING = 1, S_OPEN = 2 };

struct CommandInfo {
    const char* name;
    const char* args;
    const char* desc;
};

const CommandInfo kCommands[] = {
    {"help", "", "list commands"},
    {"clear", "", "clear the console log"},
    {"echo", "<text>", "print text back"},
    {"map", "<room>", "travel to a room by name"},
    {"list", "", "list all rooms"},
    {"restart", "", "restart the game"},
    {"reload", "", "reload the current room"},
    {"fps", "", "show the current frame rate"},
    {"timescale", "<value>", "set simulation speed (1 = normal, 0 = paused)"},
    {"quit", "", "close the game"},
    {"exit", "", "close the game"},
};

int g_state = S_CLOSED;

struct LogLine {
    std::string text;
    unsigned int color;
};
std::vector<LogLine> g_log;
int g_scroll = 0;

std::string g_input;
size_t g_cursor = 0;

std::vector<std::string> g_history;
int g_history_pos = -1;

double g_blink_t = 0.0;
double g_fps_smoothed = 0.0;

constexpr int kAtlasW = 512;
constexpr int kAtlasH = 512;
constexpr int kFirstChar = 32;
constexpr int kNumChars = 96;
constexpr float kFontSize = 13.0f;

stbtt_bakedchar g_con_chars[kNumChars];
unsigned int g_con_tex = 0;
float g_con_line_height = kFontSize * 1.2f;
bool g_con_font_ready = false;

void ensure_console_font() {
    if (g_con_font_ready) return;
    g_con_font_ready = true;

    std::vector<unsigned char> bitmap(kAtlasW * kAtlasH);
    stbtt_BakeFontBitmap(g_vcr_ttf_data, 0, kFontSize, bitmap.data(), kAtlasW, kAtlasH, kFirstChar,
                         kNumChars, g_con_chars);

    std::vector<unsigned char> rgba(kAtlasW * kAtlasH * 4);
    for (int i = 0; i < kAtlasW * kAtlasH; ++i) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = bitmap[i];
    }
    g_con_tex = render_upload_texture(rgba.data(), kAtlasW, kAtlasH);
}

float con_text_width(const std::string& s) {
    ensure_console_font();
    float x = 0, y = 0;
    for (unsigned char c : s) {
        if (c < kFirstChar || c >= kFirstChar + kNumChars) {
            x += kFontSize * 0.5f;
            continue;
        }
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(g_con_chars, kAtlasW, kAtlasH, c - kFirstChar, &x, &y, &q, 1);
    }
    return x;
}

void con_draw_text(double x, double y, const std::string& s, unsigned int color, double alpha) {
    ensure_console_font();
    float px = (float)x;
    float py = (float)y + kFontSize;
    for (unsigned char c : s) {
        if (c < kFirstChar || c >= kFirstChar + kNumChars) {
            px += kFontSize * 0.5f;
            continue;
        }
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(g_con_chars, kAtlasW, kAtlasH, c - kFirstChar, &px, &py, &q, 1);
        render_draw_glyph_colored(g_con_tex, q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0, q.s0, q.t0, q.s1,
                                  q.t1, color, alpha);
    }
}

void log_line(const std::string& s, unsigned int color = kColLog) {
    g_log.push_back({s, color});
    if (g_log.size() > 200) g_log.erase(g_log.begin());
    g_scroll = 0;
}

int find_room_by_name(const std::string& name) {
    for (int i = 0; i < g_room_count_rt; ++i)
        if (g_room_defs_rt[i].name && name == g_room_defs_rt[i].name) return i;
    return -1;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void print_help() {
    log_line("commands:");
    for (const auto& c : kCommands) {
        char buf[128];
        std::string sig = c.args[0] ? (std::string(c.name) + " " + c.args) : std::string(c.name);
        std::snprintf(buf, sizeof(buf), "  %-22s - %s", sig.c_str(), c.desc);
        log_line(buf);
    }
}

void execute_command(const std::string& line) {
    log_line("] " + line);

    size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos) return;
    size_t sep = line.find_first_of(" \t", a);
    std::string cmd = line.substr(a, sep == std::string::npos ? std::string::npos : sep - a);
    std::string rest;
    if (sep != std::string::npos) {
        size_t b = line.find_first_not_of(" \t", sep);
        if (b != std::string::npos) rest = line.substr(b);
    }
    std::string lc = to_lower(cmd);

    if (lc == "help") {
        print_help();
    } else if (lc == "clear") {
        g_log.clear();
    } else if (lc == "echo") {
        log_line(rest);
    } else if (lc == "map") {
        if (rest.empty()) {
            log_line("usage: map <room>", kColError);
        } else {
            int idx = find_room_by_name(rest);
            if (idx < 0)
                log_line("unknown room: " + rest, kColError);
            else {
                g_pending_room = idx;
                log_line("travelling to " + rest);
            }
        }
    } else if (lc == "list") {
        log_line("rooms:");
        for (int i = 0; i < g_room_count_rt; ++i) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "  %d: %s%s", i, g_room_defs_rt[i].name,
                         i == g_current_room ? "  (current)" : "");
            log_line(buf);
        }
    } else if (lc == "restart") {
        log_line("restarting game...");
        g_game_restart_requested = true;
    } else if (lc == "reload") {
        log_line("reloading room...");
        g_pending_room = g_current_room;
    } else if (lc == "fps") {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "fps: %.1f (target %.1f)", g_fps_smoothed, g_room_speed_v);
        log_line(buf);
    } else if (lc == "timescale") {
        if (rest.empty()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "timescale: %.3f", g_timescale);
            log_line(buf);
        } else {
            double v = std::atof(rest.c_str());
            if (v < 0.0) v = 0.0;
            g_timescale = v;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "timescale set to %.3f", v);
            log_line(buf);
        }
    } else if (lc == "quit" || lc == "exit") {
        log_line("bye.");
        g_game_end_requested = true;
    } else {
        log_line("unknown command: " + cmd + " (type 'help')", kColError);
    }
}

void insert_char(char c) {
    g_input.insert(g_input.begin() + (long)g_cursor, c);
    g_cursor++;
}

void set_state(int s) {
    g_state = s;
    g_console_open = s != S_CLOSED;
}

}

void console_update() {
    if (render_key_pressed(VK_OEM_TILDE)) {
        if (g_state == S_CLOSED) {
            set_state(S_TYPING);
            g_input.clear();
            g_cursor = 0;
            g_history_pos = -1;
        } else if (g_state == S_TYPING) {
            set_state(S_OPEN);
        } else {
            set_state(S_CLOSED);
        }
    }

    double dt = render_delta_time();
    if (dt > 0.0)
        g_fps_smoothed = g_fps_smoothed <= 0.0 ? 1.0 / dt : g_fps_smoothed * 0.9 + (1.0 / dt) * 0.1;

    if (g_state == S_CLOSED) return;
    g_blink_t += dt > 0.0 ? dt : 0.0;

    if (render_key_pressed(VK_ESCAPE)) {
        if (!g_input.empty()) {
            g_input.clear();
            g_cursor = 0;
        } else {
            set_state(S_CLOSED);
        }
        return;
    }

    bool shift = render_key_down(VK_SHIFT);

    for (int k = 'A'; k <= 'Z'; ++k)
        if (render_key_pressed(k)) insert_char(shift ? (char)k : (char)(k - 'A' + 'a'));
    for (int k = '0'; k <= '9'; ++k)
        if (render_key_pressed(k)) insert_char((char)k);
    if (render_key_pressed(VK_SPACE)) insert_char(' ');
    if (render_key_pressed(VK_OEM_MINUS)) insert_char(shift ? '_' : '-');
    if (render_key_pressed(VK_OEM_PERIOD)) insert_char('.');

    if (render_key_pressed(VK_BACKSPACE) && g_cursor > 0) {
        g_input.erase(g_cursor - 1, 1);
        g_cursor--;
    }
    if (render_key_pressed(VK_DELETE) && g_cursor < g_input.size()) g_input.erase(g_cursor, 1);
    if (render_key_pressed(VK_LEFT) && g_cursor > 0) g_cursor--;
    if (render_key_pressed(VK_RIGHT) && g_cursor < g_input.size()) g_cursor++;
    if (render_key_pressed(VK_HOME)) g_cursor = 0;
    if (render_key_pressed(VK_END)) g_cursor = g_input.size();

    if (render_key_pressed(VK_UP) && !g_history.empty()) {
        if (g_history_pos < 0) g_history_pos = (int)g_history.size() - 1;
        else if (g_history_pos > 0) g_history_pos--;
        g_input = g_history[g_history_pos];
        g_cursor = g_input.size();
    }
    if (render_key_pressed(VK_DOWN)) {
        if (g_history_pos >= 0 && g_history_pos + 1 < (int)g_history.size()) {
            g_history_pos++;
            g_input = g_history[g_history_pos];
        } else {
            g_history_pos = -1;
            g_input.clear();
        }
        g_cursor = g_input.size();
    }

    if (g_state == S_OPEN) {
        int page = shift ? 5 : 1;
        int max_scroll = std::max(0, (int)g_log.size() - 1);
        if (render_key_pressed(VK_PAGE_UP)) g_scroll = std::min(max_scroll, g_scroll + page);
        if (render_key_pressed(VK_PAGE_DOWN)) g_scroll = std::max(0, g_scroll - page);
    }

    if (render_key_pressed(VK_ENTER)) {
        if (!g_input.empty()) {
            auto existing = std::find(g_history.begin(), g_history.end(), g_input);
            if (existing != g_history.end()) g_history.erase(existing);
            g_history.push_back(g_input);
            execute_command(g_input);
        }
        g_input.clear();
        g_cursor = 0;
        g_history_pos = -1;
        if (g_state == S_TYPING) set_state(S_CLOSED);
    }
}

namespace {

void draw_input_row(float x0, float w, float y) {
    render_set_color(0x000000);
    render_set_alpha(0.7);
    render_draw_rectangle(x0, y - 6, x0 + w, y + g_con_line_height, false);
    render_set_alpha(1.0);
    render_set_color(kColBorder);
    render_draw_rectangle(x0, y - 6, x0 + w, y - 4, false);

    std::string prompt = " > " + g_input;
    con_draw_text(x0 + 4, y, prompt, kColInput, 1.0);

    float caret_x = x0 + 4 + con_text_width(" > " + g_input.substr(0, g_cursor));
    if (std::fmod(g_blink_t, 1.0) < 0.5) con_draw_text(caret_x, y, "_", kColInput, 1.0);
}

}

void console_draw() {
    if (g_state == S_CLOSED) return;
    ensure_console_font();

    int gw = render_gui_width();
    int gh = render_gui_height();
    float line_h = g_con_line_height;

    unsigned int saved_col = render_get_color();
    double saved_alpha = render_get_alpha();
    int saved_halign = render_get_halign();
    int saved_valign = render_get_valign();

    render_set_halign(0);
    render_set_valign(0);

    if (g_state == S_OPEN) {
        int panel_h = (int)(gh * 0.35f);

        render_set_color(0x000000);
        render_set_alpha(0.7);
        render_draw_rectangle(0, 0, gw, panel_h, false);
        render_set_alpha(1.0);
        render_set_color(kColBorder);
        render_draw_rectangle(0, panel_h, gw, panel_h + 2, false);

        float input_y = panel_h - line_h - 4;
        float y = input_y - line_h - 4;
        int max_shown = std::max(0, (int)((y + line_h) / line_h));
        int shown = 0;
        int skip = g_scroll;
        for (auto it = g_log.rbegin(); it != g_log.rend() && shown < max_shown; ++it) {
            if (skip > 0) { --skip; continue; }
            con_draw_text(6, y, it->text, it->color, 1.0);
            y -= line_h;
            ++shown;
        }

        draw_input_row(0, (float)gw, input_y);
    } else {
        float input_y = gh - line_h - 8;
        draw_input_row(0, (float)gw, input_y);
    }

    render_set_color(saved_col);
    render_set_alpha(saved_alpha);
    render_set_halign(saved_halign);
    render_set_valign(saved_valign);
}

}
