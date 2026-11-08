// Copyright (c) 2026, i2kiselev
// BSD 3-Clause License: see LICENSE in the repository root.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <rex/graphics/startup_pipeline_compilation.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>

using namespace std::chrono_literals;
using rex::graphics::RunStartupPipelineCompilation;
using rex::graphics::ShaderStorageInitialization;
using rex::graphics::StoredPipelineShadersPresent;

TEST_CASE("Startup compiler completes all work with one or several workers", "[shader_startup]") {
  const size_t workers = GENERATE(1u, 4u);
  ShaderStorageInitialization operation;
  std::atomic<size_t> compiled{0};
  size_t saved_completed = 0;
  RunStartupPipelineCompilation(
      11, workers, operation,
      [&](size_t) {
        ++compiled;
        return true;
      },
      [&] { saved_completed = operation.GetSnapshot().completed; });
  REQUIRE(compiled == 11);
  REQUIRE(operation.GetSnapshot().completed == 11);
  REQUIRE(saved_completed == 11);
}

TEST_CASE("Startup compiler saves while its sole worker is in a driver call", "[shader_startup]") {
  ShaderStorageInitialization operation;
  std::mutex mutex;
  std::condition_variable condition;
  bool release = false;
  size_t saves = 0;
  auto work = std::async(std::launch::async, [&] {
    RunStartupPipelineCompilation(
        2, 1, operation,
        [&](size_t index) {
          if (index == 0)
            return true;
          std::unique_lock lock(mutex);
          condition.wait(lock, [&] { return release; });
          return true;
        },
        [&] {
          std::lock_guard lock(mutex);
          ++saves;
          condition.notify_all();
        },
        10ms);
  });
  std::unique_lock lock(mutex);
  const bool periodic = condition.wait_for(lock, 2s, [&] { return saves >= 2; });
  const size_t completed = operation.GetSnapshot().completed;
  release = true;
  lock.unlock();
  condition.notify_all();
  work.get();
  REQUIRE(periodic);
  REQUIRE(completed == 1);
  REQUIRE(operation.GetSnapshot().completed == 2);
  REQUIRE(saves >= 3);
}

TEST_CASE("Startup compiler cancellation drains calls without claiming completion",
          "[shader_startup]") {
  ShaderStorageInitialization operation;
  size_t calls = 0;
  RunStartupPipelineCompilation(
      10, 1, operation,
      [&](size_t) {
        ++calls;
        operation.RequestCancel();
        return true;
      },
      [] {});
  operation.Finish();
  REQUIRE(calls == 1);
  REQUIRE(operation.GetSnapshot().completed == 1);
  REQUIRE(operation.GetSnapshot().phase == ShaderStorageInitialization::Phase::kCancelled);
}

TEST_CASE("Startup compiler failed or throwing work stops further assignment", "[shader_startup]") {
  ShaderStorageInitialization operation;
  const bool throws = GENERATE(false, true);
  size_t calls = 0;
  RunStartupPipelineCompilation(
      10, 1, operation,
      [&](size_t) {
        ++calls;
        if (throws)
          throw std::runtime_error("driver fault");
        return false;
      },
      [] {});
  operation.Finish();
  REQUIRE(calls == 1);
  REQUIRE(operation.GetSnapshot().failed == 1);
  REQUIRE(operation.GetSnapshot().completed == 0);
  REQUIRE(operation.GetSnapshot().phase == ShaderStorageInitialization::Phase::kFailed);
}

TEST_CASE("Startup compiler accepts empty or already cancelled work", "[shader_startup]") {
  ShaderStorageInitialization operation;
  size_t count = 0;
  SECTION("cancelled") {
    count = 10;
    operation.RequestCancel();
  }
  size_t calls = 0;
  RunStartupPipelineCompilation(
      count, 1, operation,
      [&](size_t) {
        ++calls;
        return true;
      },
      [] {});
  REQUIRE(calls == 0);
}

TEST_CASE("Stored pipelines need their shaders in storage to start up", "[shader_startup]") {
  struct Description {
    uint64_t vertex_shader_hash;
    uint64_t pixel_shader_hash;
  };
  const std::set<uint64_t> stored{0xA, 0xB};
  const auto in_storage = [&](uint64_t hash) { return stored.count(hash) != 0; };
  REQUIRE(StoredPipelineShadersPresent(Description{0xA, 0xB}, in_storage));
  REQUIRE(StoredPipelineShadersPresent(Description{0xA, 0}, in_storage));
  // The game makes some shaders only while it runs: their pipelines wait until then.
  REQUIRE_FALSE(StoredPipelineShadersPresent(Description{0xC, 0xB}, in_storage));
  REQUIRE_FALSE(StoredPipelineShadersPresent(Description{0xA, 0xC}, in_storage));
}
