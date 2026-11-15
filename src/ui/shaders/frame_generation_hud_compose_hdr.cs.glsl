#version 460
// frame_generation_hud_compose.cs.glsl for an scRGB swapchain (present_hdr): puts the
// HUD layer (upscale_hud_layer.cs.glsl, straight alpha, SDR code values) over a
// presented frame, rendered or generated, at the paper white, as hdr_output.ps.glsl
// shows the HUD of a frame without frame generation.
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;
layout(push_constant) uniform FrameGenerationHudComposeConstants {
  uvec2 size;
  uint swap_red_blue;  // Unused: the frame is RGBA.
  float paper_white_scale;  // Paper white nits / 80.
  // 0 decodes the HUD as piecewise sRGB, otherwise as this power (present_hdr_gamma).
  float gamma;
} constants;
layout(set = 0, binding = 0) uniform sampler2D hud_layer;
layout(set = 0, binding = 1, rgba16f) uniform image2D frame;

vec3 Decode(vec3 code) {
  code = clamp(code, 0.0, 1.0);
  if (constants.gamma > 0.0) {
    return pow(code, vec3(constants.gamma));
  }
  return mix(pow((code + 0.055) / 1.055, vec3(2.4)), code / 12.92,
             lessThanEqual(code, vec3(0.04045)));
}

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, constants.size))) {
    return;
  }
  vec4 hud = texelFetch(hud_layer, ivec2(pixel), 0);
  if (hud.a <= 0.0) {
    return;
  }
  vec4 color = imageLoad(frame, ivec2(pixel));
  vec3 rgb = mix(color.rgb, Decode(hud.rgb) * constants.paper_white_scale, hud.a);
  imageStore(frame, ivec2(pixel), vec4(rgb, color.a));
}
