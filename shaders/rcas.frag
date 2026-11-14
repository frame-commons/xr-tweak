// SPDX-License-Identifier: MIT
#version 450
// RCAS (Robust Contrast-Adaptive Sharpening) after AMD FidelityFX FSR 1.0, ffx_fsr1.h
// (MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc.) -- the same filter OpenXR Toolkit
// offers as "FSR sharpening". Re-expressed in GLSL for one fragment per output pixel:
//   * 5-tap cross (centre e, b up, d left, f right, h down), texelFetch with clamped coordinates
//   * the negative lobe is the largest that keeps every channel inside [min,max] of the cross,
//     capped at RCAS_LIMIT, then scaled by the user's strength (FSR's exp2(-stops))
//   * alpha is passed through untouched (an ALPHA_BLEND app's passthrough mask must survive)
// srgb != 0: the sampled view is an _SRGB format, so texelFetch returned LINEAR values; RCAS runs in
// the encoded domain FSR expects, and the result is decoded again before the _SRGB attachment re-encodes.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
    float strength;   // 0..1
    int   srgb;
    int   max_x;      // width-1
    int   max_y;      // height-1
} pc;
layout(location = 0) out vec4 o;

vec3 enc(vec3 c) { return pc.srgb != 0 ? mix(c * 12.92, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0/2.4)) - 0.055, step(vec3(0.0031308), c)) : c; }
vec3 dec(vec3 c) { return pc.srgb != 0 ? mix(c / 12.92, pow((max(c, vec3(0.0)) + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)) : c; }

vec3 tap(ivec2 p) {
    return enc(texelFetch(src, clamp(p, ivec2(0), ivec2(pc.max_x, pc.max_y)), 0).rgb);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 ce = texelFetch(src, clamp(p, ivec2(0), ivec2(pc.max_x, pc.max_y)), 0);
    vec3 e = enc(ce.rgb);
    vec3 b = tap(p + ivec2(0, -1));
    vec3 d = tap(p + ivec2(-1, 0));
    vec3 f = tap(p + ivec2(1, 0));
    vec3 h = tap(p + ivec2(0, 1));
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    const float RCAS_LIMIT = 0.25 - (1.0 / 16.0);
    // FSR: hitMin = min(mn4, e) / (4 * mx4); hitMax = (1 - max(mx4, e)) / (4 * mn4 - 4)
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1e-5));
    vec3 hitMax = (vec3(1.0) - max(mx4, e)) / min(4.0 * mn4 - vec3(4.0), vec3(-1e-5));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.strength;
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    vec3 outc = (lobe * (b + d + f + h) + e) * rcpL;
    o = vec4(dec(clamp(outc, 0.0, 1.0)), ce.a);
}
