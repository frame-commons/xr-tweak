#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""test_cli.py — bin/xr-tweak's config verbs and install guard, no docker, no headset.

Run: python3 test/test_cli.py

Everything runs under a throwaway HOME, so a run on the headset cannot touch the live config.
The CLI's `effective()` mirrors src/config.cpp; the same INI cases as test/test_config.cpp are asserted
here so the two cannot drift apart silently.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

CLI = pathlib.Path(__file__).resolve().parents[1] / "bin" / "xr-tweak"
FAILED = []


def check(name, cond, detail=""):
    print(("  ok   " if cond else "  FAIL ") + name + ("" if cond else f"  {detail}"))
    if not cond:
        FAILED.append(name)


def run(home, *args):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("XR_", "XDG_"))}
    env["HOME"] = str(home)
    r = subprocess.run([sys.executable, str(CLI), *args], env=env, capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def main():
    with tempfile.TemporaryDirectory() as h:
        home = pathlib.Path(h)
        rc, so, _ = run(home, "show", "Godot Engine", "--json")
        e = json.loads(so)
        check("no config = all defaults", rc == 0 and e["scale"] == 1.0 and e["sharpen"] == 0.0 and e["enabled"])
        run(home, "set", "default", "scale=1.1", "sharpen=0.2")
        rc, so, _ = run(home, "set", "Godot Engine", "scale=1.5", "sharpen=0.4", "--json")
        e = json.loads(so)
        check("app section wins over default", e["scale"] == 1.5 and e["sharpen"] == 0.4, so)
        check("sections reported", e["sections"] == "[default][app:Godot Engine]", e["sections"])
        e = json.loads(run(home, "show", "Godot", "--json")[1])
        check("exact app match only", e["scale"] == 1.1, e)
        cfg = (home / ".config/xr-tweak/config.ini").read_text()
        check("config written with the section name the layer reads", "[app:Godot Engine]" in cfg, cfg)
        rc, _, err = run(home, "set", "default", "sharpen=3")
        check("out-of-range sharpen refused", rc != 0 and "sharpen" in err, err)
        rc, _, err = run(home, "set", "default", "bogus=1")
        check("unknown key refused", rc != 0, err)
        rc, so, _ = run(home, "unset", "Godot Engine", "sharpen", "--json")
        check("unset one key falls back to default", json.loads(so)["sharpen"] == 0.2, so)
        run(home, "unset", "Godot Engine")
        check("unset whole section", "[app:Godot Engine]" not in (home / ".config/xr-tweak/config.ini").read_text())

        # install guard: a library of the wrong arch is refused without --force
        fake = home / "fake.so"
        hdr = bytearray(b"\x7fELF" + b"\0" * 60)
        machine = os.uname().machine
        hdr[18:20] = (0x3E if machine == "aarch64" else 0xB7).to_bytes(2, "little")
        fake.write_bytes(bytes(hdr))
        rc, _, err = run(home, "install", "--lib", str(fake))
        check("wrong-arch install refused", rc != 0 and "--force" in err, err)
        check("refused install wrote no manifest",
              not (home / ".local/share/openxr/1/api_layers/implicit.d/xr_tweak.json").exists())
        rc, so, _ = run(home, "install", "--lib", str(fake), "--force", "--json")
        man = home / ".local/share/openxr/1/api_layers/implicit.d/xr_tweak.json"
        m = json.loads(man.read_text()) if man.exists() else {}
        check("forced install writes a manifest with the kill switch",
              m.get("api_layer", {}).get("disable_environment") == "XR_TWEAK_DISABLE", m)
        check("manifest points at the installed copy",
              m.get("api_layer", {}).get("library_path") == str(home / ".local/share/xr-tweak/libxr_tweak.so"), m)
        st = json.loads(run(home, "status", "--json")[1])
        check("status sees it", st["installed"] and st["library_arch"] in ("aarch64", "x86_64"), st)
        run(home, "uninstall")
        check("uninstall removes manifest + lib", not man.exists() and not (home / ".local/share/xr-tweak/libxr_tweak.so").exists())
        (home / ".local/state/xr-tweak").mkdir(parents=True)
        (home / ".local/state/xr-tweak/xr-tweak.log").write_text(
            '2026-10-23T12:00:00 pid=1 app="Beat Saber" engine="Unity" config=/x sections=none enabled=1 scale=1.000 '
            'width=0 height=0 sharpen=0.00\n2026-10-23T12:00:01 pid=1 app="Beat Saber" view0 recommended 1x1 -> 1x1\n')
        rows = json.loads(run(home, "apps", "--json")[1])
        check("apps parses the layer's log line", [r["app"] for r in rows] == ["Beat Saber"], rows)

        # foveation key: names only, aliases canonicalised, junk refused, valve is the default
        e = json.loads(run(home, "show", "x", "--json")[1])
        check("foveation defaults to valve", e["foveation"] == "valve", e)
        rc, so, _ = run(home, "set", "some-game", "foveation=med", "--json")
        e = json.loads(so)
        check("foveation=med stored as medium", rc == 0 and e["foveation"] == "medium", so)
        check("written canonically", "foveation = medium" in (home / ".config/xr-tweak/config.ini").read_text())
        rc, _, err = run(home, "set", "some-game", "foveation=ultra")
        check("foveation junk refused", rc != 0 and "valve, off, low, medium, high" in err, err)
        rc, _, err = run(home, "set", "some-game", "foveation=3")
        check("foveation number refused (names only)", rc != 0, err)
        rc, so, _ = run(home, "set", "some-game", "foveation=off")
        check("set prints the not-yet-seen note", "applies only if the app uses XR_FB_foveation" in so, so)
        # an app the layer saw WITHOUT XR_FB_foveation gets the stronger warning
        (home / ".local/state/xr-tweak/xr-tweak.log").write_text(
            '2026-10-23T12:00:00 pid=1 app="Alyx" engine="Source2" config=/x sections=none enabled=1 scale=1.000 '
            'width=0 height=0 sharpen=0.00 foveation=valve fb_foveation=0 fb_configuration=0 valve_fdm=above(/usr/share/'
            'openxr/1/api_layers/implicit.d) layers_below=[]\n')
        rc, so, _ = run(home, "set", "Alyx", "foveation=high")
        check("non-FB app: set warns it will do nothing + names Valve's toggle",
              "will do nothing" in so and "Foveated Rendering" in so, so)
        rows = json.loads(run(home, "apps", "--json")[1])
        check("apps splits out fb_foveation + valve_fdm",
              rows[0]["fb_foveation"] == "0" and rows[0]["valve_fdm"].startswith("above("), rows)
        rc, so, _ = run(home, "set", "Alyx", "foveation=valve")
        check("control: foveation=valve prints no warning", "NOTE" not in so and "note:" not in so, so)
    print(f"test_cli: {'FAIL' if FAILED else 'ok'} ({len(FAILED)} failed)")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
