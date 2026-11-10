// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 450

// Copies a present frame to an HDR swapchain image (or an HDR frame to an SDR one), converting
// its transfer function and primaries. Frames hold either SDR colour (BT.709, gamma 2.2) or a
// Switch 2 game's HDR10 output (BT.2020, PQ).

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

layout(binding = 0) uniform sampler2D frame_texture;

// Bytes 0..15 are full_screen_triangle.vert's tex_scale and tex_offset.
layout(push_constant) uniform PushConstants {
    layout(offset = 16) uint source; // 0: SDR, 1: HDR10
    uint target;                     // 0: SDR, 1: HDR10, 2: scRGB (linear BT.709, 1.0 = 80 nits)
    float paper_white;               // nits of SDR white
    float peak;                      // nits the display shows at most
};

const uint SOURCE_HDR10 = 1u;
const uint TARGET_SDR = 0u;
const uint TARGET_HDR10 = 1u;
const uint TARGET_SCRGB = 2u;

// Linear BT.709 <-> BT.2020 (column-major).
const mat3 BT709_TO_BT2020 = mat3(0.6274040, 0.0690970, 0.0163916,
                                  0.3292820, 0.9195400, 0.0880132,
                                  0.0433136, 0.0113612, 0.8955950);
const mat3 BT2020_TO_BT709 = mat3(1.6604910, -0.1245505, -0.0181508,
                                  -0.5876411, 1.1328999, -0.1005789,
                                  -0.0728499, -0.0083494, 1.1187297);

// SMPTE ST 2084.
const float PQ_M1 = 0.1593017578125;
const float PQ_M2 = 78.84375;
const float PQ_C1 = 0.8359375;
const float PQ_C2 = 18.8515625;
const float PQ_C3 = 18.6875;

vec3 PqToNits(vec3 e) {
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / PQ_M2));
    return 10000.0 * pow(max(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), vec3(1.0 / PQ_M1));
}

vec3 NitsToPq(vec3 nits) {
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(PQ_M1));
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), vec3(PQ_M2));
}

// Keeps colour below 0.8 of SDR white and rolls brighter colour off towards white (C1 at the
// knee), so an HDR frame shown on an SDR swapchain keeps its highlights instead of clipping.
vec3 RollOff(vec3 x) {
    const float knee = 0.8;
    float m = max(max(x.r, x.g), x.b);
    if (m <= knee) {
        return x;
    }
    float t = (m - knee) / (1.0 - knee);
    return x * ((knee + (1.0 - knee) * t / (1.0 + t)) / m);
}

void main() {
    vec4 texel = texture(frame_texture, texcoord);
    if ((source == SOURCE_HDR10) == (target == TARGET_HDR10) && target != TARGET_SCRGB) {
        // Already in the swapchain's encoding.
        color = vec4(texel.rgb, 1.0);
        return;
    }
    // Linear BT.709 light in nits; negative where BT.2020 colour lies outside BT.709.
    vec3 nits;
    if (source == SOURCE_HDR10) {
        nits = BT2020_TO_BT709 * PqToNits(texel.rgb);
    } else {
        // Gamma 2.2 rather than the piecewise sRGB curve: what SDR displays show.
        nits = pow(max(texel.rgb, 0.0), vec3(2.2)) * paper_white;
    }
    if (target == TARGET_HDR10) {
        color = vec4(NitsToPq(BT709_TO_BT2020 * nits), 1.0);
    } else if (target == TARGET_SDR) {
        vec3 sdr = RollOff(max(nits, 0.0) / paper_white);
        color = vec4(pow(min(sdr, 1.0), vec3(1.0 / 2.2)), 1.0);
    } else {
        color = vec4(min(nits, peak) / 80.0, 1.0);
    }
}
