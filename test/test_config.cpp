// SPDX-License-Identifier: MIT
// Unit test for the config parser: section precedence, exact app match, clamping, junk tolerance.
#include "config.h"
#include <cmath>
#include <cstdio>
#include <string>
static int fails = 0;
#define EXPECT(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
int main() {
    const char *ini =
        "# comment\n"
        "[app:Godot Engine]\n scale = 1.5 \n sharpen=0.4\n"
        "[default]\nscale=1.1\nsharpen = 0.2\nbogus = 7\n"
        "[app:Other]\nenabled=0\nwidth=1234\n"
        "[app:Clamp]\nsharpen=9\nscale=100\n"
        "[app:FovHigh]\nfoveation = high\n"
        "[app:FovJunk]\nfoveation=ultra\n"
        "[app:FovOff]\nfoveation=off\n";
    TweakSettings d = tweak_parse(ini, "Nobody");
    EXPECT(std::fabs(d.scale - 1.1) < 1e-9 && std::fabs(d.sharpen - 0.2) < 1e-9 && d.enabled);
    EXPECT(d.source == "[default]");
    TweakSettings g = tweak_parse(ini, "Godot Engine");          // app section wins although it comes first
    EXPECT(std::fabs(g.scale - 1.5) < 1e-9 && std::fabs(g.sharpen - 0.4) < 1e-9);
    EXPECT(g.source == "[default][app:Godot Engine]");
    TweakSettings p = tweak_parse(ini, "Godot");                 // exact match only
    EXPECT(std::fabs(p.scale - 1.1) < 1e-9);
    TweakSettings o = tweak_parse(ini, "Other");
    EXPECT(!o.enabled && o.width == 1234 && std::fabs(o.scale - 1.1) < 1e-9);
    TweakSettings c = tweak_parse(ini, "Clamp");
    EXPECT(c.sharpen == 1.0 && std::fabs(c.scale - 1.1) < 1e-9); // sharpen clamped, absurd scale ignored
    EXPECT(d.foveation == -1);                                   // default: leave it to Valve / the app
    EXPECT(tweak_parse(ini, "FovHigh").foveation == 3);
    EXPECT(tweak_parse(ini, "FovOff").foveation == 0);
    EXPECT(tweak_parse(ini, "FovJunk").foveation == -1);         // unknown word ignored, not "off"
    EXPECT(tweak_parse("[default]\nfoveation=low\n[app:A]\nfoveation=valve\n", "A").foveation == -1);
    EXPECT(tweak_parse("[default]\nfoveation=med\n", "B").foveation == 2);
    EXPECT(tweak_foveation_parse("medium") == 2 && tweak_foveation_parse("") == -2);
    EXPECT(std::string(tweak_foveation_name(3)) == "high" && std::string(tweak_foveation_name(-1)) == "valve");
    TweakSettings e = tweak_parse("", "x");
    EXPECT(e.enabled && e.scale == 1.0 && e.sharpen == 0.0 && e.source.empty());
    printf("test_config: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
