#version 450

//============================================================================================================
//
//
//                  Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================

// Snapdragon Game Super Resolution 2, fast variant, Convert pass: the 2-pass compute
// variant's, without the colour the fast Upscale reads straight from the input
// (SnapdragonStudios/snapdragon-gsr, sgsr/v2/include/glsl_2_pass_cs/sgsr2_convert.comp).
// ShiftGlue changes: Vulkan bindings in one set and push constants instead of the uniform
// buffer; the motion comes from the game's motion vectors (UV units, previous position =
// current + motion; VulkanUpscalerInputs) for every pixel instead of encoded velocity or a depth
// reprojection; depth is flipped where 1 is near; pixels outside the render size are skipped.

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

layout(set = 0, binding = 0) uniform sampler2D InputColor;
layout(set = 0, binding = 1) uniform sampler2D InputDepth;
layout(set = 0, binding = 2) uniform sampler2D InputVelocity;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D MotionDepthClipAlphaBuffer;
layout(set = 0, binding = 4, r32ui) uniform writeonly uimage2D YCoCgColor;

// 0 near, 1 far, as the pass was written for.
vec4 Depth(vec4 gathered) {
  return params.depthInverted != 0u ? vec4(1.0) - gathered : gathered;
}

void main() {
  uvec2 InputPos = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(InputPos, params.renderSize))) {
    return;
  }
  float Exposure_co_rcp = params.preExposure;
  vec2 ViewportSizeInverse = params.renderSizeRcp;

  vec2 gatherCoord = vec2(InputPos) * ViewportSizeInverse;

  // Derived from ffx_fsr2_reconstruct_dilated_velocity_and_previous_depth.h: FindNearestDepth.
  vec4 topleft = Depth(textureGather(InputDepth, gatherCoord, 0));
  vec2 v10 = vec2(ViewportSizeInverse.x * 2.0, 0.0);
  vec4 topRight = Depth(textureGather(InputDepth, gatherCoord + v10, 0));
  vec2 v12 = vec2(0.0, ViewportSizeInverse.y * 2.0);
  vec4 bottomLeft = Depth(textureGather(InputDepth, gatherCoord + v12, 0));
  vec2 v14 = vec2(ViewportSizeInverse.x * 2.0, ViewportSizeInverse.y * 2.0);
  vec4 bottomRight = Depth(textureGather(InputDepth, gatherCoord + v14, 0));
  float maxC = min(min(min(topleft.y, topRight.x), bottomLeft.z), bottomRight.w);
  float topleft4 = min(min(min(topleft.y, topleft.x), topleft.z), topleft.w);

  float depthclip = 0.0;
  if (maxC < 1.0 - 1.0e-05f) {
    float topRight4 = min(min(min(topRight.y, topRight.x), topRight.z), topRight.w);
    float bottomLeft4 = min(min(min(bottomLeft.y, bottomLeft.x), bottomLeft.z), bottomLeft.w);
    float bottomRight4 =
        min(min(min(bottomRight.y, bottomRight.x), bottomRight.z), bottomRight.w);

    float Wdepth = 0.0;
    float Ksep = 1.37e-05f;
    float Kfov = params.cameraFovAngleHor;
    float diagonal_length = length(vec2(params.renderSize));
    float Ksep_Kfov_diagonal = Ksep * Kfov * diagonal_length;

    float Depthsep = Ksep_Kfov_diagonal * (1.0 - maxC);
    float EPSILON = 1.19e-07f;
    Wdepth += clamp((Depthsep / (abs(maxC - topleft4) + EPSILON)), 0.0, 1.0);
    Wdepth += clamp((Depthsep / (abs(maxC - topRight4) + EPSILON)), 0.0, 1.0);
    Wdepth += clamp((Depthsep / (abs(maxC - bottomLeft4) + EPSILON)), 0.0, 1.0);
    Wdepth += clamp((Depthsep / (abs(maxC - bottomRight4) + EPSILON)), 0.0, 1.0);
    depthclip = clamp(1.0f - Wdepth * 0.25, 0.0, 1.0);
  }

  // The pass's motion is the screen position (2 * UV - 1, y down) minus the previous one: from
  // the game's UV-unit motion, previous = current + motion, it is -2 * motion.
  vec2 motion = -2.0 * texelFetch(InputVelocity, ivec2(InputPos), 0).xy;

  imageStore(MotionDepthClipAlphaBuffer, ivec2(InputPos), vec4(motion, depthclip, 0.0));
}
