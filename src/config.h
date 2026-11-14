// SPDX-License-Identifier: MIT
// xr-tweak config: one INI file, sections keyed by OpenXR applicationName.
//
//   [default]            applies to every app
//   enabled = 1          0 = the layer is a pure passthrough for this app
//   scale   = 1.0        multiplies recommendedImageRectWidth/Height (clamped to the max rect)
//   width   = 0          explicit per-eye width  (overrides scale when > 0)
//   height  = 0          explicit per-eye height (overrides scale when > 0)
//   sharpen = 0.0        RCAS strength 0..1 (0 = off, 1 = FSR1 RCAS at 0 stops)
//   foveation = valve    valve | off | low | medium | high. Only for apps that call XR_FB_foveation
//                        themselves: rewrites the level in their xrCreateFoveationProfileFB. "valve" =
//                        untouched (Valve's fdm_injection layer and the app decide). Apps without
//                        XR_FB_foveation cannot be reached from an OpenXR layer; the log says so.
//
//   [app:Godot Engine]   overrides for one app; key is XrApplicationInfo::applicationName, exact
//   scale = 1.2
//
// Lookup: $XR_TWEAK_CONFIG, else $XDG_CONFIG_HOME/xr-tweak/config.ini, else ~/.config/xr-tweak/config.ini.
// Unknown keys are ignored (logged), a missing file means "all defaults" = passthrough.
#pragma once
#include <string>

struct TweakSettings {
    bool enabled = true;
    double scale = 1.0;
    int width = 0;
    int height = 0;
    double sharpen = 0.0;
    int foveation = -1;   // -1 = leave to Valve/the app; else XrFoveationLevelFB (0 none .. 3 high)
    std::string source;   // which sections applied, for the log line
};

// Parse `text` and return the settings for `app` ([default] then [app:<app>]).
// "valve" -> -1, "off" -> 0, "low" -> 1, "medium"/"med" -> 2, "high" -> 3; anything else -> -2 (invalid)
int tweak_foveation_parse(const std::string &v);
const char *tweak_foveation_name(int level);
TweakSettings tweak_parse(const std::string &text, const std::string &app);
// Read the file at the standard location (or $XR_TWEAK_CONFIG) and parse it.
TweakSettings tweak_load(const std::string &app, std::string *path_out);
std::string tweak_config_path();
