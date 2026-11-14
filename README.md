# xr-tweak

Per-app OpenXR settings for the Valve Steam Frame: render resolution, sharpening, and the foveation level.

xr-tweak is an implicit OpenXR API layer for Linux (aarch64 for the Frame's native apps, plus an x86_64
build for x86 apps run through FEX). It sits between an OpenXR app and the runtime and changes only what
its config file asks for, per app. With no config file, every call passes straight through.

**Status: early.** All tests pass off the headset (see "Tests"). On a Frame, the layer loads into native
OpenXR apps, the foveation level reaches the runtime, and sharpening runs on the app's swapchains (its cost is
below). Resolution override has been tested off the headset only. Expect rough edges and report them.

## What it does

| Feature | How | Config key |
|---|---|---|
| Resolution | Rewrites `recommendedImageRectWidth/Height` in `xrEnumerateViewConfigurationViews`, rounded and clamped to the runtime's maximum (at least 16 px). | `scale`, or `width` + `height` |
| Sharpening | AMD FidelityFX FSR 1.0 RCAS, in place, on every swapchain the app submits in a projection layer, inside `xrReleaseSwapchainImage`, on the app's own Vulkan queue. | `sharpen` 0..1 |
| Foveation level | For apps that use `XR_FB_foveation` themselves: rewrites the level in `xrCreateFoveationProfileFB`, and adds `XR_FB_foveation_configuration` if the app lacks it. `off` also turns dynamic foveation off. | `foveation` = `valve` (default, untouched) / `off` / `low` / `medium` / `high` |
| Off switches | `enabled = 0` for one app; `XR_TWEAK_DISABLE` in an app's environment; uninstall. | |

Foveation for apps that do **not** call `XR_FB_foveation` cannot be reached from an OpenXR layer. On the
Frame, Valve's own implicit layer (`XrApiLayer_VALVE_fdm_injection`) handles those apps. Its only control
is the Steam client's per-game **Properties > Foveated Rendering** checkbox, which xr-tweak cannot drive.

## What we measured

- **Sharpening strength is steep.** RCAS divides by `4·lobe + 1`, so the effect is far from linear in the
  dial. On the test's blurred step, `sharpen = 0.3` changed the image by 2 levels in total and `1.0` by 38.
  Start around 0.6 to 0.8.
- **Sharpening keeps edges honest.** On the same step, the steepest step went from 32 to 38 levels with
  the 10-90 % rise no wider, the plateaus untouched, alpha byte-identical, and no ringing beyond 3 % of
  the step. RCAS is robust, not halo-free: one run measured a 2-level overshoot (194 over 192).
- **Foveation saved 0.3 ms in the one scene we measured.** In a Godot test field on the Frame (90 Hz panel,
  worn, one run per level), the median GPU time per frame was 12.48 ms with foveation off, 12.75 ms at high
  and 12.46 ms with the app's own level. That is a 2 % spread, with high the slowest. The scene's cost is
  most likely geometry and shadow passes, which foveation does not reduce. Other scenes may gain more; we
  have not measured one.
- **With sharpening on, the app's GPU time per frame rose by about 5 ms on the Frame (a first reading).** In the same test field,
  at the Frame's 1728x1728 per eye in sRGB, the median GPU time per frame went from 13.44 ms with sharpening off to
  18.70 ms at `sharpen = 0.7`. That is one pair of one-minute runs, not interleaved, with the headset on a stand,
  so treat it as a first reading. That scene already ran at half rate (45 of 90 fps) either way. For an app that
  just makes 90 fps, this is enough to drop it to half rate.
- **Not judged yet:** how sharpening looks on the Frame.

## Install

You need Docker on a Linux x86_64 PC to build, and SSH access to the Frame (or a shell on it).

```sh
./build.sh build              # out/aarch64/libxr_tweak.so and out/x86_64/libxr_tweak.so
```

Copy the repository (with `out/`) to the Frame, then on the Frame, as your normal user:

```sh
bin/xr-tweak install          # the layer for this machine's arch, plus its manifest
bin/xr-tweak status           # installed? which library, the config, the last log lines
bin/xr-tweak uninstall
```

Everything goes under your home directory (no root, nothing on the read-only system partition):

| What | Where |
|---|---|
| layer | `~/.local/share/xr-tweak/libxr_tweak.so` |
| manifest | `~/.local/share/openxr/1/api_layers/implicit.d/xr_tweak.json` |
| config | `~/.config/xr-tweak/config.ini` (`$XR_TWEAK_CONFIG` overrides) |
| log | `~/.local/state/xr-tweak/xr-tweak.log` |

## Use

```sh
bin/xr-tweak apps                            # applicationNames the layer has seen; key your sections on these
bin/xr-tweak set default sharpen=0.6         # every app
bin/xr-tweak set "some-game" scale=1.2 sharpen=0.8
bin/xr-tweak set "some-game" foveation=high  # XR_FB_foveation apps only
bin/xr-tweak set "some-game" enabled=0       # loaded, but does nothing for this app
bin/xr-tweak show "some-game" --json         # the effective settings
bin/xr-tweak unset "some-game"
```

Settings are read once, when the app creates its OpenXR instance, so a change applies from the app's next
launch. The config file is plain INI; sections match `applicationName` exactly and `[default]` applies first:

```ini
[default]
sharpen = 0.5

[app:some-game]
scale = 1.2
sharpen = 0.8
```

**Turning it off for one launch:** the manifest names `disable_environment: XR_TWEAK_DISABLE`, so the
OpenXR loader skips the layer whenever that variable is set to anything (even `0`; that is the loader's
rule). For a Steam game, use the launch options `XR_TWEAK_DISABLE=1 %command%`.

## How it works

**Sharpening.** When the app calls `xrReleaseSwapchainImage`, the image it just finished is, by
`XR_KHR_vulkan_enable`'s rule, in `COLOR_ATTACHMENT_OPTIMAL` and owned by the app's queue, which the
runtime (and so a layer) may use during that call. xr-tweak records one command buffer: copy the image to
a layer-owned intermediate, then one full-screen fragment pass per array layer that reads the copy with a
5-tap cross and writes RCAS back into the swapchain image. The image ends in the layout it arrived in, and
the release is forwarded.

- Only swapchains named by an `XrCompositionLayerProjection` in an earlier `xrEndFrame` are touched. Quad
  layers (menus, text) and depth swapchains never are, and a new swapchain's first frame is not sharpened.
  The layer adds `TRANSFER_SRC` to colour swapchains at creation, and falls back to the app's own flags,
  unsharpened, if the runtime refuses.
- A fragment pass, not compute, because `_SRGB` formats are colour-attachment capable everywhere and
  storage capable almost nowhere. For `_SRGB` the shader re-encodes before RCAS and decodes after.
- Alpha passes through unchanged, so a passthrough app's room mask survives.
- Any Vulkan error turns sharpening off for the rest of that session and logs why; the app carries on.
- Not supported: OpenGL/GLES sessions (logged, no sharpening), MSAA swapchains, cube maps, mip chains.
- Sharpening on top of foveation is untested: RCAS over low-density foveation tiles may make the
  periphery blockier. If it does, use `sharpen = 0` for that app.

**Foveation.** For an app that enabled `XR_FB_foveation`, every `xrCreateFoveationProfileFB` is forwarded
with the configured `XrFoveationLevelFB` in place of the app's. The app's structs are never written: the
head struct and everything up to the level struct are copied and the rest of the chain is shared, so a
`XR_META_foveation_eye_tracked` struct the app chained still reaches the runtime. An unrecognised struct
ahead of the level struct means the request passes through unchanged and the log says so. `valve`, the
default, hooks nothing.

**Layer order.** The Khronos loader lists system implicit layers before the ones in `~/.local/share`, and
the first listed is nearest the app. Valve's layer is installed in `/usr/share`, so it sits above
xr-tweak: what it does to an app's calls happens before xr-tweak sees them. Every app start logs where it
found Valve's layer (`valve_fdm=above|below|absent`), and `bin/xr-tweak apps` shows it.

## Logs and privacy

xr-tweak writes one line when an app starts (its name, engine, the sections that matched and the values
applied) and one line per notable event. Nothing per frame. The lines go to stderr and to
`~/.local/state/xr-tweak/xr-tweak.log`, which is not rotated; delete it whenever you like. That is
everything it writes. **It sends nothing over the network and has no telemetry.**

## Tests

```sh
./build.sh test
```

The tests run in the same Docker image as the build (Debian bookworm, the Khronos OpenXR-SDK 1.1.63
loader built from source, Mesa's lavapipe for Vulkan), with no headset and no GPU. `test/run_tests.py`
installs the layer with `bin/xr-tweak` into a throwaway home and drives the real loader against a stub
runtime (`test/stub_runtime.cpp`) and a small Vulkan app (`test/test_app.cpp`) that renders a blurred step
and reads it back. It checks:

- resolution: scaling, clamping to the runtime's maximum, per-app sections over `[default]`;
- sharpening on UNORM and on a two-layer sRGB array: the image changes, edges get steeper without
  widening, plateaus and alpha are untouched, and ringing stays small;
- controls that must come back byte-identical: no config, quad-layer-only swapchains,
  `XR_TWEAK_DISABLE`, `enabled = 0`, and after uninstall;
- a second manifest pointing at the other arch's library: the loader skips it and the app still runs;
- foveation, judged by what the stub runtime received (never by the layer's own log): each level, the
  default pass-through, an app without `_configuration`, a runtime that refuses it, an unknown struct, a
  chained eye-tracked struct, and the layer order against a stand-in for Valve's layer
  (`test/fake_valve_layer.cpp`).

`test/test_cli.py` tests `bin/xr-tweak` alone (the config verbs, the wrong-arch install refusal, the
manifest, the log parsing) without Docker. The config parser has its own unit test (`test/test_config.cpp`).
GitHub Actions runs `./build.sh test` on every push and pull request.

## Prior art

- **[OpenXR Toolkit](https://github.com/mbucchia/OpenXR-Toolkit)** (MIT) is the tool this one imitates:
  per-app resolution, FSR/NIS/CAS sharpening and foveated rendering as an OpenXR layer. It is Windows-only
  (Direct3D 11 and 12), and its author discontinued it in 2024.
- **[vrperfkit](https://github.com/fholger/vrperfkit)** and **[openvr_fsr](https://github.com/fholger/openvr_fsr)**
  by Holger Frydrych bring FSR, NIS and CAS upscaling and sharpening (and, in vrperfkit, fixed foveation
  on NVIDIA cards) to OpenVR and Oculus games on Windows, by a DLL placed next to the game. They are
  Direct3D 11 only.
- **[vkBasalt](https://github.com/DadSchoorse/vkBasalt)** (zlib) is a Vulkan post-processing layer for
  Linux with CAS. It works on a flat swapchain and knows nothing of OpenXR's projection layers.
- **[linux-vulkan-quadviews-openxr-layer](https://github.com/artscout/linux-vulkan-quadviews-openxr-layer)**
  (MIT) is a Linux OpenXR layer for quad views and eye-tracked foveation with CAS-style sharpening, for
  apps that ask for quad views.
- **[SteamFrame-upscale](https://github.com/TBog/SteamFrame-upscale)** is a Frame OpenXR layer that
  denoises and upscales the monochrome passthrough stream, not the app's own image.

We found no Linux OpenXR layer that offers per-app resolution, sharpening of the app's image and a
foveation level together (searched 2026-11-14). xr-tweak's RCAS is AMD's, re-expressed in GLSL.

## Credits

The sharpening filter is RCAS from [AMD FidelityFX Super Resolution 1.0](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
(MIT; its notice is in [LICENSE](LICENSE)). The OpenXR loader and headers are Khronos's
[OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) (Apache-2.0), used at build and test time only.

## License

MIT. See [LICENSE](LICENSE).

Made with help from Claude.
