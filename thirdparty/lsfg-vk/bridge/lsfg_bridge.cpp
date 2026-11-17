/**
 * liblsfg_framegen.so's C interface (lsfg_bridge.h) over lsfg-vk's framegen.
 *
 * The shader table and the DLL resource walk follow lsfg-vk's
 * src/extract/extract.cpp (MIT, lsfg-vk); the mipmaps pass uses lsfg-vk-android's
 * replacement shader, as its layer does by default, since the DLL's translated
 * one miscompiles on Mesa's Adreno driver.
 */

#include "lsfg_bridge.h"

#include <cstdio>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

#include <pe-parse/parse.h>

#include "lsfg_3_1.hpp"
#include "lsfg_3_1p.hpp"
#include "mipmaps_replacement_spv.h"
#include "trans.hpp"

namespace {

// Shader name: the DLL's RCDATA resource holding its Direct3D bytecode.
const std::unordered_map<std::string, uint32_t> kShaderResources = {
    {"mipmaps", 255},    {"alpha[0]", 267},   {"alpha[1]", 268},   {"alpha[2]", 269},
    {"alpha[3]", 270},   {"beta[0]", 275},    {"beta[1]", 276},    {"beta[2]", 277},
    {"beta[3]", 278},    {"beta[4]", 279},    {"gamma[0]", 257},   {"gamma[1]", 259},
    {"gamma[2]", 260},   {"gamma[3]", 261},   {"gamma[4]", 262},   {"delta[0]", 257},
    {"delta[1]", 263},   {"delta[2]", 264},   {"delta[3]", 265},   {"delta[4]", 266},
    {"delta[5]", 258},   {"delta[6]", 271},   {"delta[7]", 272},   {"delta[8]", 273},
    {"delta[9]", 274},   {"generate", 256},   {"p_mipmaps", 255},  {"p_alpha[0]", 290},
    {"p_alpha[1]", 291}, {"p_alpha[2]", 292}, {"p_alpha[3]", 293}, {"p_beta[0]", 298},
    {"p_beta[1]", 299},  {"p_beta[2]", 300},  {"p_beta[3]", 301},  {"p_beta[4]", 302},
    {"p_gamma[0]", 280}, {"p_gamma[1]", 282}, {"p_gamma[2]", 283}, {"p_gamma[3]", 284},
    {"p_gamma[4]", 285}, {"p_delta[0]", 280}, {"p_delta[1]", 286}, {"p_delta[2]", 287},
    {"p_delta[3]", 288}, {"p_delta[4]", 289}, {"p_delta[5]", 281}, {"p_delta[6]", 294},
    {"p_delta[7]", 295}, {"p_delta[8]", 296}, {"p_delta[9]", 297}, {"p_generate", 256},
};

std::unordered_map<uint32_t, std::vector<uint8_t>> g_resources;
std::unordered_map<std::string, std::vector<uint8_t>> g_spirv;
bool g_performance = false;
bool g_initialized = false;

void SetError(char* error, size_t error_size, const std::string& message) {
  if (error && error_size) {
    std::snprintf(error, error_size, "%s", message.c_str());
  }
}

int OnResource(void*, const peparse::resource& resource) {
  if (resource.type == peparse::RT_RCDATA && resource.buf && resource.buf->bufLen > 0) {
    g_resources[resource.name].assign(resource.buf->buf,
                                      resource.buf->buf + resource.buf->bufLen);
  }
  return 0;
}

bool ReadDll(const char* path, std::string& error) {
  if (!g_resources.empty()) {
    return true;
  }
  peparse::parsed_pe* dll = peparse::ParsePEFromFile(path);
  if (!dll) {
    error = std::string("cannot read ") + path;
    return false;
  }
  peparse::IterRsrc(dll, OnResource, nullptr);
  peparse::DestructParsedPE(dll);
  for (const auto& [name, resource] : kShaderResources) {
    if (!g_resources.count(resource)) {
      g_resources.clear();
      error = "shader " + name + " missing from Lossless.dll (an older Lossless Scaling?)";
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> LoadShader(const std::string& name) {
  if (name == "mipmaps" || name == "p_mipmaps") {
    return std::vector<uint8_t>(mipmaps_replacement_spv,
                                mipmaps_replacement_spv + mipmaps_replacement_spv_len);
  }
  auto cached = g_spirv.find(name);
  if (cached != g_spirv.end()) {
    return cached->second;
  }
  std::vector<uint8_t> spirv = Extract::translateShader(g_resources.at(kShaderResources.at(name)));
  g_spirv.emplace(name, spirv);
  return spirv;
}

}  // namespace

extern "C" {

__attribute__((visibility("default"))) int lsfg_bridge_init(const LsfgBridgeInit* init,
                                                            char* error, size_t error_size) {
  try {
    std::string reason;
    if (!ReadDll(init->dll_path, reason)) {
      SetError(error, error_size, reason);
      return 0;
    }
    g_performance = init->performance != 0;
    // framegen takes the flow scale's inverse.
    const float flow = 1.0f / init->flow_scale;
    if (g_performance) {
      LSFG_3_1P::initializeExternal(init->get_instance_proc_addr, init->instance,
                                    init->physical_device, init->device, init->queue_family,
                                    init->queue, init->hdr != 0, flow, init->generations,
                                    LoadShader);
    } else {
      LSFG_3_1::initializeExternal(init->get_instance_proc_addr, init->instance,
                                   init->physical_device, init->device, init->queue_family,
                                   init->queue, init->hdr != 0, flow, init->generations,
                                   LoadShader);
    }
    g_initialized = true;
    return 1;
  } catch (const std::exception& exception) {
    SetError(error, error_size, exception.what());
    return 0;
  }
}

__attribute__((visibility("default"))) int32_t lsfg_bridge_create(
    VkImage input_0, VkImage input_1, const VkImage* outputs, uint32_t output_count,
    uint32_t width, uint32_t height, VkFormat format, char* error, size_t error_size) {
  try {
    const std::vector<VkImage> output_images(outputs, outputs + output_count);
    const VkExtent2D extent = {width, height};
    return g_performance
               ? LSFG_3_1P::createContextFromImages(input_0, input_1, output_images, extent,
                                                    format)
               : LSFG_3_1::createContextFromImages(input_0, input_1, output_images, extent,
                                                   format);
  } catch (const std::exception& exception) {
    SetError(error, error_size, exception.what());
    return -1;
  }
}

__attribute__((visibility("default"))) int lsfg_bridge_present(int32_t context, char* error,
                                                               size_t error_size) {
  try {
    const std::vector<int> no_semaphores;
    if (g_performance) {
      LSFG_3_1P::presentContext(context, -1, no_semaphores);
    } else {
      LSFG_3_1::presentContext(context, -1, no_semaphores);
    }
    return 1;
  } catch (const std::exception& exception) {
    SetError(error, error_size, exception.what());
    return 0;
  }
}

__attribute__((visibility("default"))) void lsfg_bridge_delete(int32_t context) {
  try {
    if (g_performance) {
      LSFG_3_1P::deleteContext(context);
    } else {
      LSFG_3_1::deleteContext(context);
    }
  } catch (const std::exception&) {
  }
}

__attribute__((visibility("default"))) void lsfg_bridge_finalize() {
  if (!g_initialized) {
    return;
  }
  try {
    if (g_performance) {
      LSFG_3_1P::finalize();
    } else {
      LSFG_3_1::finalize();
    }
  } catch (const std::exception&) {
  }
  g_initialized = false;
}

}  // extern "C"
