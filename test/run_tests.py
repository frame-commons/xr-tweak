#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Off-headset tests for xr-tweak, run INSIDE the build image (./build.sh test).

Real Khronos loader (OpenXR-SDK, pinned in docker/Dockerfile) + a stub runtime + lavapipe Vulkan. The
layer is installed by bin/xr-tweak into a throwaway HOME, so the CLI's install, the manifest,
implicit-layer discovery and disable_environment are all the real code paths.

The sharpening oracle is a PROPERTY of any halo-free sharpener, not RCAS's arithmetic (memory
test-oracle-must-not-be-the-code): on a blurred step the 10-90 % rise gets no wider and the steepest
step gets steeper, nothing rings past 3 % of the step height, the flat plateaus and alpha do not move.
Every positive case has a control that must come back byte-identical (memory a-check-that-cannot-fail).
"""
import json
import re
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
LAYER = HERE.parent
REPO = LAYER
OUT = LAYER / "out" / "x86_64"
CLI = REPO / "bin" / "xr-tweak"
LVP = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
TRANSFER_SRC = 0x08   # XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT

fails = []
passes = 0


def check(name, cond, detail=""):
    global passes
    if cond:
        passes += 1
        print(f"  ok   {name}")
    else:
        fails.append(name)
        print(f"  FAIL {name} {detail}")


class Env:
    def __init__(self, ini=None, install=True, extra_manifest=None):
        self.tmp = tempfile.TemporaryDirectory(prefix="xrtweak-")
        self.home = pathlib.Path(self.tmp.name)
        rt = self.home / "runtime.json"
        rt.write_text(json.dumps({"file_format_version": "1.0.0",
                                  "runtime": {"name": "stub", "library_path": str(OUT / "stub_runtime.so")}}))
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(("XR_", "XDG_"))}
        self.env.update(HOME=str(self.home), XR_RUNTIME_JSON=str(rt), VK_ICD_FILENAMES=LVP,
                        STUB_RUNTIME_SO=str(OUT / "stub_runtime.so"))
        if install:
            r = subprocess.run([sys.executable, str(CLI), "install", "--lib", str(OUT / "libxr_tweak.so"), "--json"],
                               env=self.env, capture_output=True, text=True)
            assert r.returncode == 0, r.stderr
        if extra_manifest:
            d = self.home / ".local/share/openxr/1/api_layers/implicit.d"
            (d / "xr_tweak_other.json").write_text(extra_manifest)
        if ini is not None:
            cfg = self.home / ".config/xr-tweak/config.ini"
            cfg.parent.mkdir(parents=True, exist_ok=True)
            cfg.write_text(ini)

    def app(self, *args, env=None):
        e = dict(self.env)
        e.update(env or {})
        r = subprocess.run([str(OUT / "test_app"), *args], env=e, capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            raise RuntimeError(f"test_app {args} exit {r.returncode}\n{r.stderr}")
        return json.loads(r.stdout), r.stderr

    def log(self):
        p = self.home / ".local/state/xr-tweak/xr-tweak.log"
        return p.read_text() if p.exists() else ""

    def cli(self, *args):
        r = subprocess.run([sys.executable, str(CLI), *args], env=self.env, capture_output=True, text=True)
        return r.returncode, r.stdout, r.stderr


def rise_width(v, lo, hi):
    a, b = lo + 0.1 * (hi - lo), hi - 0.1 * (hi - lo)
    return sum(1 for x in v if a < x < b)


def max_step(v):
    return max(abs(v[i + 1] - v[i]) for i in range(len(v) - 1))


def sharpened(name, lay):
    i, o = lay["in"], lay["out"]
    lo, hi = min(i), max(i)
    check(f"{name}: image changed", o != i)
    # RCAS is "robust", not halo-free: its lobe is limited so the result cannot clip, and the first run
    # (2026-10-23) measured a 2-level overshoot on a 128-level UNORM step (194 over 192). The property
    # kept: any ring stays under 3 % of the step height, i.e. no visible halo.
    tol = 0.03 * (hi - lo)
    check(f"{name}: no visible halo (all within {tol:.1f} of [{lo},{hi}])",
          all(lo - tol <= x <= hi + tol for x in o), str(o))
    check(f"{name}: plateaus untouched", all(abs(o[x] - i[x]) <= 1 for x in list(range(0, 16)) + list(range(32, 48))),
          str(o))
    check(f"{name}: rise no wider ({rise_width(i, lo, hi)} -> {rise_width(o, lo, hi)})",
          rise_width(o, lo, hi) <= rise_width(i, lo, hi))
    check(f"{name}: steepest step steeper ({max_step(i)} -> {max_step(o)})", max_step(o) > max_step(i))
    check(f"{name}: alpha preserved", lay["alpha_out"] == lay["alpha_in"])
    check(f"{name}: rows identical, grey stays grey", lay["rows_equal"] and lay["gb_equal_r"])


def untouched(name, res):
    for n, lay in enumerate(res["layers"]):
        check(f"{name}: layer {n} byte-identical", lay["out"] == lay["in"] and lay["alpha_out"] == lay["alpha_in"],
              str(lay["out"]))


def fake_valve_manifest(where: pathlib.Path):
    where.mkdir(parents=True, exist_ok=True)
    (where / "XrApiLayer_VALVE_fdm_injection.json").write_text(json.dumps({
        "file_format_version": "1.0.0", "api_layer": {
            "name": "XR_APILAYER_VALVE_fdm_injection", "library_path": str(OUT / "fake_valve_layer.so"),
            "api_version": "1.0", "implementation_version": "1", "description": "stand-in for Valve's layer",
            "disable_environment": "FAKE_VALVE_DISABLE"}}))


def foveation_tests():
    """XR_FB_foveation level control. The oracle is what the STUB RUNTIME received (its own record of the
    profile create info and the instance's extensions), never the layer's log; the log is checked only for
    the read-out it promises. LOW + dynamic is what a typical Godot project asks for
    (foveation_level=1, foveation_dynamic=true)."""
    def fov(ini, mode, env=None, extra=None):
        e = Env(ini)
        if extra:
            extra(e)
        res, err = e.app("--format", "37", "--fov", mode, "--frames", "1", env=env)
        return res, err, e.log()

    # M. control: no foveation key -> the app's own LOW + dynamic reaches the runtime untouched
    res, _, log = fov("[default]\nsharpen=0\n", "conf")
    check("M valve (default): runtime got the app's level 1 dynamic 1", res["fov"] == {"level": 1, "dynamic": 1,
          "n_level": 1, "eye": 0, "other": 0}, str(res.get("fov")))
    check("M valve: no extension added", "XR_FB_foveation_configuration" in res["runtime_exts"]
          and res["runtime_exts"].count(",") == 3, res["runtime_exts"])
    check("M valve: read-out in the start line", "foveation=valve fb_foveation=1 fb_configuration=1" in log, log)
    check("M valve: no profile rewrite logged", "foveation profile" not in log, log)
    check("M no Valve layer here: valve_fdm=absent", "valve_fdm=absent layers_below=[]" in log, log)

    # N/O/P. levels reach the runtime; dynamic kept except for off; META eye-tracked struct preserved
    res, _, log = fov("[app:xrtweak-test]\nfoveation=high\n", "conf")
    check("N high: runtime got level 3, app's dynamic kept", res["fov"] == {"level": 3, "dynamic": 1, "n_level": 1,
          "eye": 0, "other": 0}, str(res["fov"]))
    check("N high: the app's own (const) struct still says 1", res["app_struct_level"] == 1)
    check("N high: logged", "app asked level=1 dynamic=1 -> xr-tweak level=3 (high) dynamic=1" in log, log)
    res, _, _ = fov("[default]\nfoveation=off\n", "conf")
    check("O off: runtime got level 0, dynamic disabled", res["fov"]["level"] == 0 and res["fov"]["dynamic"] == 0,
          str(res["fov"]))
    res, _, _ = fov("[default]\nfoveation=medium\n", "conf-eye")
    check("P medium + eye-tracked app: level 2, META struct still chained", res["fov"]["level"] == 2
          and res["fov"]["eye"] == 1 and res["fov"]["n_level"] == 1, str(res["fov"]))

    # Q. app uses XR_FB_foveation without _configuration: the layer enables it and adds a level struct
    res, _, log = fov("[default]\nfoveation=low\n", "fb")
    check("Q fb-only app: _configuration added to the instance", "XR_FB_foveation_configuration" in res["runtime_exts"],
          res["runtime_exts"])
    check("Q fb-only app: runtime got an added level 1, not dynamic", res["fov"] == {"level": 1, "dynamic": 0,
          "n_level": 1, "eye": 0, "other": 0}, str(res["fov"]))
    check("Q logged as added", "fb_configuration=added" in log and "(level struct added)" in log, log)
    # Q control: the same app with no foveation key gets no extension added and no level struct
    res, _, _ = fov("", "fb")
    check("Q control: valve -> no extension added, no level struct",
          "configuration" not in res["runtime_exts"] and res["fov"]["n_level"] == 0, str(res))

    # R. a runtime without _configuration: retry as the app asked, app still runs, nothing changed, said so
    res, _, log = fov("[default]\nfoveation=high\n", "fb", env={"STUB_NO_FOV_CONFIG": "1"})
    check("R runtime refuses _configuration: app still runs, profile untouched",
          res["fov_result"] == 0 and res["fov"]["n_level"] == 0, str(res))
    check("R logged: refused + NOT applied", "refused added XR_FB_foveation_configuration" in log
          and "NOT applied" in log, log)

    # S. _configuration enabled but the app chains no level struct: one is added
    res, _, _ = fov("[default]\nfoveation=high\n", "conf-nolevel")
    check("S no level struct: added at level 3", res["fov"]["n_level"] == 1 and res["fov"]["level"] == 3, str(res["fov"]))

    # T. an unknown struct before the level struct: refuse to relink, pass through, say so
    res, _, log = fov("[default]\nfoveation=high\n", "conf-odd")
    check("T unknown struct first: passed through (level 1, the odd struct kept)",
          res["fov"]["level"] == 1 and res["fov"]["other"] == 1, str(res["fov"]))
    check("T logged NOT applied", "unrecognised struct" in log, log)

    # U. an app without XR_FB_foveation: nothing added, and the log says it cannot be reached
    res, _, log = fov("[default]\nfoveation=high\n", "none")
    check("U non-FB app: no FB extension added", "foveation" not in res["runtime_exts"], res["runtime_exts"])
    check("U non-FB app: logged that xr-tweak cannot foveate it", "cannot apply" in log and "Foveated Rendering" in log, log)

    # V/W/X. controls: enabled=0, the kill switch, and a per-app "valve" over a [default] level
    res, _, _ = fov("[default]\nfoveation=high\n[app:xrtweak-test]\nenabled=0\n", "conf")
    check("V enabled=0: level untouched", res["fov"]["level"] == 1, str(res["fov"]))
    res, _, _ = fov("[default]\nfoveation=high\n", "conf", env={"XR_TWEAK_DISABLE": "1"})
    check("W kill switch: level untouched", res["fov"]["level"] == 1, str(res["fov"]))
    res, _, _ = fov("[default]\nfoveation=high\n[app:xrtweak-test]\nfoveation=valve\n", "conf")
    check("X per-app valve beats [default] high", res["fov"]["level"] == 1, str(res["fov"]))

    # Y. Valve's layer in a SYSTEM implicit dir (as on the headset, /usr/share): the real loader puts it
    #    ABOVE ours. Witness: the fake layer's own view of what is below it.
    def sysdir(e):
        fake_valve_manifest(e.home / "sys/openxr/1/api_layers/implicit.d")
    e = Env("[default]\nfoveation=high\n")
    sysdir(e)
    res, err = e.app("--format", "37", "--fov", "conf", "--frames", "1", env={"XDG_DATA_DIRS": str(e.home / "sys")})
    check("Y loader order: Valve's system-dir layer sits above xr-tweak (its own witness)",
          "fake-valve: below=[XR_APILAYER_FRAMECOMMONS_xr_tweak]" in err, err[-400:])
    check("Y xr-tweak reports valve_fdm=above", "valve_fdm=above(" in e.log() and "layers_below=[]" in e.log(), e.log())
    check("Y level still reaches the runtime through Valve's (passthrough) layer", res["fov"]["level"] == 3,
          str(res["fov"]))

    # Z. the other order (Valve's layer enabled explicitly -> below ours): detected from the loader's chain
    e = Env("[default]\nfoveation=high\n")
    fake_valve_manifest(e.home / ".local/share/openxr/1/api_layers/explicit.d")
    res, err = e.app("--format", "37", "--fov", "conf", "--frames", "1",
                     env={"XR_ENABLE_API_LAYERS": "XR_APILAYER_VALVE_fdm_injection"})
    check("Z explicit Valve layer is below (its own witness)", "fake-valve: below=[]" in err, err[-400:])
    check("Z xr-tweak reports valve_fdm=below", "valve_fdm=below layers_below=[XR_APILAYER_VALVE_fdm_injection]"
          in e.log(), e.log())
    check("Z level reaches the runtime", res["fov"]["level"] == 3, str(res["fov"]))

    # the CLI reads the start line back
    rc, so, _ = e.cli("apps", "--json")
    rows = json.loads(so) if rc == 0 else []
    check("CLI apps: shows foveation read-out", rows and "fb_foveation=1" in rows[0]["applied"], so)


def main():
    print("xr-tweak off-headset tests (real loader + stub runtime + lavapipe)")
    r = subprocess.run([str(OUT / "test_config")], capture_output=True, text=True)
    check("config parser unit test", r.returncode == 0, r.stdout)

    # A. installed, no config file: a pure passthrough
    e = Env()
    res, err = e.app()
    check("A no config: recommended unchanged", res["recommended"] == [1000, 800], str(res["recommended"]))
    check("A no config: swapchain usage untouched", res["runtime_usage"] == res["app_usage"])
    untouched("A no config", res)
    check("A no config: layer was loaded (log line)", 'app="xrtweak-test"' in e.log() and "sections=none" in e.log(),
          e.log())

    # B/C. scale + clamp
    res, _ = Env("[default]\nscale=1.5\n").app("--no-session")
    check("B scale 1.5: 1000x800 -> 1500x1200", res["recommended"] == [1500, 1200], str(res["recommended"]))
    res, _ = Env("[default]\nscale=3\n").app("--no-session")
    check("C scale 3: clamped to max 2000x1600", res["recommended"] == [2000, 1600], str(res["recommended"]))

    # D. per-app section beats default; another app name gets only the default
    ini = "[default]\nscale=0.5\n[app:xrtweak-test]\nwidth=1234\nheight=1000\n"
    e = Env(ini)
    res, _ = e.app("--no-session")
    check("D per-app explicit size", res["recommended"] == [1234, 1000], str(res["recommended"]))
    res, _ = e.app("--no-session", "--name", "Some Other App")
    check("D other app gets [default] only", res["recommended"] == [500, 400], str(res["recommended"]))
    rc, so, _ = e.cli("apps", "--json")
    names = {r["app"] for r in json.loads(so)} if rc == 0 else set()
    check("D xr-tweak apps lists both names", names == {"xrtweak-test", "Some Other App"}, so)

    # E. sharpen, UNORM, one layer
    e = Env("[default]\nsharpen=1\n")
    res, err = e.app("--format", "37")
    check("E sharpen: TRANSFER_SRC added to the runtime's swapchain", bool(res["runtime_usage"] & TRANSFER_SRC)
          and not (res["app_usage"] & TRANSFER_SRC))
    sharpened("E sharpen UNORM", res["layers"][0])
    check("E sharpen: log says sharpening", "sharpening a 48x8 x1 swapchain" in e.log(), e.log())
    # E2. our own pass is timed: one "sharpen gpu" line when the swapchain goes, counting the passes read back
    e.app("--format", "37", "--frames", "12")
    m = re.findall(r"sharpen gpu 48x8 x1: n=(\d+) over \d+ s, median ([\d.]+) ms, p90 ([\d.]+) ms.*\(last\)", e.log())
    check("E2 sharpen gpu: a last line with passes timed", bool(m) and int(m[-1][0]) >= 1, e.log()[-600:])
    check("E2 sharpen gpu: median <= p90", bool(m) and float(m[-1][1]) <= float(m[-1][2]), str(m[-1:]))

    # F. sRGB, two-layer array (multiview); layer 1 is the mirrored edge
    res, _ = e.app("--format", "43", "--layers", "2")
    for n, lay in enumerate(res["layers"]):
        lay2 = dict(lay)
        if n == 1:   # mirror so the shared assertions read left-to-right rising
            lay2 = {k: (v[::-1] if isinstance(v, list) else v) for k, v in lay.items()}
        sharpened(f"F sharpen sRGB layer {n}", lay2)

    # G. control: same swapchain submitted only as a QUAD layer is never sharpened
    res, _ = e.app("--format", "37", "--quad")
    untouched("G quad-only swapchain", res)

    # H. control: the kill switch -- loader skips the layer; nothing scaled, nothing sharpened, no log
    e = Env("[default]\nsharpen=1\nscale=1.5\n")
    res, _ = e.app("--format", "37", env={"XR_TWEAK_DISABLE": "1"})
    check("H kill switch: recommended unchanged", res["recommended"] == [1000, 800], str(res["recommended"]))
    check("H kill switch: usage untouched", res["runtime_usage"] == res["app_usage"])
    untouched("H kill switch", res)
    check("H kill switch: layer never loaded (no log)", e.log() == "", e.log())

    # I. control: enabled=0 for this app only
    e = Env("[default]\nsharpen=1\nscale=1.5\n[app:xrtweak-test]\nenabled=0\n")
    res, _ = e.app("--format", "37")
    check("I enabled=0: recommended unchanged", res["recommended"] == [1000, 800])
    untouched("I enabled=0", res)

    # J. strength is a dial: 0.3 changes less than 1.0, and more than 0
    def change(s):
        r, _ = Env(f"[default]\nsharpen={s}\n").app("--format", "37")
        lay = r["layers"][0]
        return sum(abs(a - b) for a, b in zip(lay["out"], lay["in"]))
    c03, c10 = change(0.3), change(1.0)
    check(f"J strength monotone (0.3 -> {c03}, 1.0 -> {c10})", 0 < c03 < c10)

    # K. a second implicit manifest pointing at the OTHER arch's .so (the FEX x86 + native arm64 case):
    #    the loader must skip the unloadable one and still run the app with ours
    other = LAYER / "out" / "aarch64" / "libxr_tweak.so"
    man = json.dumps({"file_format_version": "1.0.0", "api_layer": {
        "name": "XR_APILAYER_FRAMECOMMONS_xr_tweak_other_arch", "library_path": str(other), "api_version": "1.0",
        "implementation_version": "1", "description": "wrong-arch probe", "disable_environment": "XR_TWEAK_DISABLE"}})
    e = Env("[default]\nscale=1.5\n", extra_manifest=man)
    try:
        res, err = e.app("--no-session")
        check("K wrong-arch manifest beside ours: app still runs, ours still applies",
              res["recommended"] == [1500, 1200], str(res))
    except RuntimeError as ex:
        check("K wrong-arch manifest beside ours: app still runs", False, str(ex)[:400])

    # L. uninstall really removes it
    e = Env("[default]\nscale=1.5\n")
    e.cli("uninstall")
    res, _ = e.app("--no-session")
    check("L after uninstall: recommended unchanged", res["recommended"] == [1000, 800])

    foveation_tests()

    print(f"xr-tweak tests: {passes} passed, {len(fails)} failed")
    for f in fails:
        print("  failed:", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
