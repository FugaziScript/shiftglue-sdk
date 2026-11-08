// Copyright (c) 2026, i2kiselev
// BSD 3-Clause License: see LICENSE in the repository root.
#pragma once

#include <chrono>
#include <functional>

#include <rex/graphics/shader_storage_initialization.h>

namespace rex::graphics {

void RunStartupPipelineCompilation(
    size_t count, size_t workers, ShaderStorageInitialization& initialization,
    const std::function<bool(size_t)>& compile, const std::function<void()>& save,
    std::chrono::milliseconds save_interval = std::chrono::seconds(10));

// A stored pipeline whose shader is not in storage cannot start up: the game makes some shaders
// only while it runs (none on the disc), and their pipelines are created when it first draws them.
template <typename Description, typename InStorage>
bool StoredPipelineShadersPresent(const Description& description, const InStorage& in_storage) {
  return in_storage(description.vertex_shader_hash) &&
         (!description.pixel_shader_hash || in_storage(description.pixel_shader_hash));
}

}  // namespace rex::graphics
