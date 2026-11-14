// SPDX-License-Identifier: MIT
#include "config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

int tweak_foveation_parse(const std::string &v) {
    if (v == "valve") return -1;
    if (v == "off" || v == "none") return 0;
    if (v == "low") return 1;
    if (v == "medium" || v == "med") return 2;
    if (v == "high") return 3;
    return -2;
}

const char *tweak_foveation_name(int level) {
    switch (level) {
        case 0: return "off";
        case 1: return "low";
        case 2: return "medium";
        case 3: return "high";
        default: return "valve";
    }
}

static void apply(TweakSettings &t, const std::string &k, const std::string &v) {
    char *end = nullptr;
    if (k == "enabled") t.enabled = !(v == "0" || v == "false" || v == "off" || v == "no");
    else if (k == "scale") { double d = strtod(v.c_str(), &end); if (end != v.c_str() && d > 0.05 && d <= 4.0) t.scale = d; }
    else if (k == "width") t.width = atoi(v.c_str());
    else if (k == "height") t.height = atoi(v.c_str());
    else if (k == "sharpen") {
        double d = strtod(v.c_str(), &end);
        if (end != v.c_str()) t.sharpen = d < 0 ? 0 : (d > 1 ? 1 : d);
    } else if (k == "foveation") {
        int f = tweak_foveation_parse(v);
        if (f >= -1) t.foveation = f;   // an unknown word leaves the previous value (default: valve)
    }
}

TweakSettings tweak_parse(const std::string &text, const std::string &app) {
    TweakSettings t;
    // two passes: [default] first, then the app's own section, so order in the file does not matter
    for (int pass = 0; pass < 2; ++pass) {
        std::istringstream in(text);
        std::string line, section;
        bool hit = false;
        while (std::getline(in, line)) {
            std::string l = trim(line);
            if (l.empty() || l[0] == '#' || l[0] == ';') continue;
            if (l[0] == '[') {
                size_t e = l.rfind(']');
                section = trim(l.substr(1, e == std::string::npos ? std::string::npos : e - 1));
                continue;
            }
            bool want = pass == 0 ? section == "default" : (section.rfind("app:", 0) == 0 && trim(section.substr(4)) == app);
            if (!want) continue;
            size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            apply(t, trim(l.substr(0, eq)), trim(l.substr(eq + 1)));
            hit = true;
        }
        if (hit) t.source += pass == 0 ? "[default]" : "[app:" + app + "]";
    }
    return t;
}

std::string tweak_config_path() {
    if (const char *p = getenv("XR_TWEAK_CONFIG")) if (*p) return p;
    if (const char *x = getenv("XDG_CONFIG_HOME")) if (*x) return std::string(x) + "/xr-tweak/config.ini";
    const char *h = getenv("HOME");
    return std::string(h ? h : "") + "/.config/xr-tweak/config.ini";
}

TweakSettings tweak_load(const std::string &app, std::string *path_out) {
    std::string path = tweak_config_path();
    if (path_out) *path_out = path;
    std::ifstream f(path);
    if (!f) return TweakSettings{};
    std::stringstream ss;
    ss << f.rdbuf();
    return tweak_parse(ss.str(), app);
}
