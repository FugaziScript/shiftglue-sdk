#version 450

//============================================================================================================
//
//
//                  Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================

// Snapdragon Game Super Resolution 2, fast variant, Upscale pass: the algorithm of the 2-pass
// fragment variant (SnapdragonStudios/snapdragon-gsr, sgsr/v2/include/glsl_2_pass_fs/
// sgsr2_upscale.fs), made for mobile GPUs: five input taps instead of nine, the colour read
// straight from the input (no YCoCg pass) and an RGB history.
// ShiftGlue changes: a compute pass with the compute variant's bindings and push constants,
// writing the guest output's RGB10A2 and the history, pixels outside the output size skipped;
// tap positions clamped to the render size.

float FastLanczos(float base)
{
	float y = base - 1.0f;
	float y2 = y * y;
	float y_temp = 0.75f * y + y2;
	return y_temp * y2;
}

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform Params {
  uvec2 renderSize;
  uvec2 displaySize;
  vec2 renderSizeRcp;
  vec2 displaySizeRcp;
  vec2 jitterOffset;
  float preExposure;
  float cameraFovAngleHor;
  float minLerpContribution;
  uint sameCameraFrmNum;
  uint reset;
  uint depthInverted;
} params;

layout(set = 0, binding = 0) uniform sampler2D PrevOutput;
layout(set = 0, binding = 1) uniform sampler2D MotionDepthClipAlphaBuffer;
layout(set = 0, binding = 2) uniform sampler2D InputColor;
layout(set = 0, binding = 3, rgb10_a2) uniform writeonly image2D SceneColorOutput;
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D HistoryOutput;

ivec2 maxPos;

vec3 Tap(ivec2 pos)
{
	return texelFetch(InputColor, clamp(pos, ivec2(0), maxPos), 0).xyz;
}

void main()
{
    if (any(greaterThanEqual(gl_GlobalInvocationID.xy, params.displaySize))) {
        return;
    }
    vec2 renderSize = vec2(params.renderSize);
    vec2 outputSize = vec2(params.displaySize);
    maxPos = ivec2(params.renderSize) - ivec2(1);
    float Biasmax_viewportXScale = min(outputSize.x / renderSize.x, 1.99);
    float scalefactor = min(20.0, pow((outputSize.x / renderSize.x) * (outputSize.y / renderSize.y), 3.0));

    vec2 Hruv = (vec2(gl_GlobalInvocationID.xy) + vec2(0.5)) * params.displaySizeRcp;
    vec2 Jitteruv;
    Jitteruv.x = clamp(Hruv.x + (params.jitterOffset.x * params.renderSizeRcp.x), 0.0, 1.0);
    Jitteruv.y = clamp(Hruv.y + (params.jitterOffset.y * params.renderSizeRcp.y), 0.0, 1.0);
    ivec2 InputPos = min(ivec2(Jitteruv * renderSize), maxPos);

    vec3 mda = textureLod(MotionDepthClipAlphaBuffer, Jitteruv, 0.0).xyz;
    vec2 Motion = mda.xy;
    vec2 PrevUV;
    PrevUV.x = clamp(-0.5 * Motion.x + Hruv.x, 0.0, 1.0);
    PrevUV.y = clamp(-0.5 * Motion.y + Hruv.y, 0.0, 1.0);
    float depthfactor = mda.z;
    vec3 HistoryColor = textureLod(PrevOutput, PrevUV, 0.0).xyz;

    vec4 Upsampledcw = vec4(0.0);
    float biasmax = Biasmax_viewportXScale;
    float biasmin = max(1.0f, 0.3 + 0.3 * biasmax);
    float biasfactor = 0.25f * depthfactor;
    float kernelbias = mix(biasmax, biasmin, biasfactor);
    float motion_viewport_len = length(Motion * outputSize);
    float curvebias = mix(-2.0, -3.0, clamp(motion_viewport_len * 0.02, 0.0, 1.0));

    vec3 rectboxcenter = vec3(0.0);
    vec3 rectboxvar = vec3(0.0);
    float rectboxweight = 0.0;
    vec2 srcpos = vec2(InputPos) + vec2(0.5) - params.jitterOffset;
    kernelbias *= 0.5f;
    float kernelbias2 = kernelbias * kernelbias;
    vec2 srcpos_srcOutputPos = srcpos - Hruv * renderSize;
    vec3 rectboxmin = vec3(1.0e30);
    vec3 rectboxmax = vec3(-1.0e30);

    const ivec2 offsets[5] = ivec2[5](ivec2(0, 1), ivec2(1, 0), ivec2(-1, 0), ivec2(0, 0),
                                      ivec2(0, -1));
    for (int i = 0; i < 5; ++i) {
        vec3 samplecolor = Tap(InputPos + offsets[i]);
        vec2 baseoffset = srcpos_srcOutputPos + vec2(offsets[i]);
        float baseoffset_dot = dot(baseoffset, baseoffset);
        float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
        float weight = FastLanczos(base);
        Upsampledcw += vec4(samplecolor * weight, weight);
        float boxweight = exp(baseoffset_dot * curvebias);
        rectboxmin = min(rectboxmin, samplecolor);
        rectboxmax = max(rectboxmax, samplecolor);
        vec3 wsample = samplecolor * boxweight;
        rectboxcenter += wsample;
        rectboxvar += (samplecolor * wsample);
        rectboxweight += boxweight;
    }

    rectboxweight = 1.0 / rectboxweight;
    rectboxcenter *= rectboxweight;
    rectboxvar *= rectboxweight;
    rectboxvar = sqrt(abs(rectboxvar - rectboxcenter * rectboxcenter));
    Upsampledcw.xyz = clamp(Upsampledcw.xyz / Upsampledcw.w, rectboxmin - vec3(0.075),
                            rectboxmax + vec3(0.075));
    Upsampledcw.w = Upsampledcw.w * (1.0f / 3.0f);
    float baseupdate = 1.0f - depthfactor;
    baseupdate = min(baseupdate, mix(baseupdate, Upsampledcw.w * 10.0f,
                                     clamp(10.0f * motion_viewport_len, 0.0, 1.0)));
    baseupdate = min(baseupdate, mix(baseupdate, Upsampledcw.w,
                                     clamp(motion_viewport_len * 0.05f, 0.0, 1.0)));
    float basealpha = baseupdate;
    const float EPSILON = 1.192e-07f;
    float boxscale = max(depthfactor, clamp(motion_viewport_len * 0.05f, 0.0, 1.0));
    float boxsize = mix(scalefactor, 1.0f, boxscale);
    vec3 sboxvar = rectboxvar * boxsize;
    vec3 boxmin = rectboxcenter - sboxvar;
    vec3 boxmax = rectboxcenter + sboxvar;
    rectboxmax = min(rectboxmax, boxmax);
    rectboxmin = max(rectboxmin, boxmin);
    vec3 clampedcolor = clamp(HistoryColor, rectboxmin, rectboxmax);
    float startLerpValue = params.minLerpContribution;
    if ((abs(mda.x) + abs(mda.y)) > 0.000001) startLerpValue = 0.0;
    float lerpcontribution = (any(greaterThan(rectboxmin, HistoryColor)) ||
                              any(greaterThan(HistoryColor, rectboxmax))) ? startLerpValue : 1.0f;
    HistoryColor = mix(clampedcolor, HistoryColor, clamp(lerpcontribution, 0.0, 1.0));
    float basemin = min(basealpha, 0.1f);
    basealpha = mix(basemin, basealpha, clamp(lerpcontribution, 0.0, 1.0));
    float alphasum = max(EPSILON, basealpha + Upsampledcw.w);
    float alpha = clamp(Upsampledcw.w / alphasum + float(params.reset), 0.0, 1.0);
    Upsampledcw.xyz = mix(HistoryColor, Upsampledcw.xyz, alpha);

    imageStore(HistoryOutput, ivec2(gl_GlobalInvocationID.xy), vec4(Upsampledcw.xyz, 0.0));
    imageStore(SceneColorOutput, ivec2(gl_GlobalInvocationID.xy), vec4(Upsampledcw.xyz, 1.0));
}
