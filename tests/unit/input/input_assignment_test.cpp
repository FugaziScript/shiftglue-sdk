#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/input/sdl/virtual_pad.h>

// The X_STATUS_* / X_ERROR_* macros cast through these unqualified, so
// FakeDriver cannot match the base class signatures without them.
using rex::X_RESULT;
using rex::X_STATUS;

using rex::input::DeviceAssignment;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::InputDriver;
using rex::input::InputSystem;
using rex::input::SharedAssignment;
using rex::input::SlotAssignment;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace {

DeviceInfo Pad(uint64_t id, uint32_t ordinal) {
  DeviceInfo info;
  info.id = static_cast<DeviceId>(id);
  info.ordinal = ordinal;
  return info;
}

std::vector<DeviceId> For(const DeviceAssignment& a, uint32_t user) {
  std::vector<DeviceId> out;
  a.DevicesForUser(user, out);
  return out;
}

/// Exercises assignment with no SDL, window, or OS behind it.
class FakeDriver final : public InputDriver {
 public:
  explicit FakeDriver(bool keyboard = false) : InputDriver(nullptr, 0), keyboard_(keyboard) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  bool is_keyboard_and_mouse() const override { return keyboard_; }

  void Add(uint64_t id, bool synthetic = false, bool non_gamepad = false) {
    DeviceInfo info;
    info.id = static_cast<DeviceId>(id);
    info.synthetic = synthetic;
    info.non_gamepad = non_gamepad;
    devices_.push_back(info);
    states_[static_cast<DeviceId>(id)] = {};
  }

  void Remove(uint64_t id) {
    std::erase_if(devices_, [&](const DeviceInfo& d) { return d.id == static_cast<DeviceId>(id); });
    states_.erase(static_cast<DeviceId>(id));
  }

  void SetButtons(uint64_t id, uint16_t buttons) {
    states_[static_cast<DeviceId>(id)].gamepad.buttons = buttons;
  }

  void QueueKeystroke(uint64_t id, uint16_t vk) {
    X_INPUT_KEYSTROKE ks = {};
    ks.virtual_key = vk;
    keystrokes_[static_cast<DeviceId>(id)] = ks;
  }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override { out = devices_; }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    auto it = states_.find(id);
    if (it == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out_state = it->second;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out) override {
    if (states_.find(id) == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out = {};
    // Stamp the id so the test can tell which device answered.
    out->sub_type = static_cast<uint8_t>(id);
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override {
    if (vibration) {
      last_left = vibration->left_motor_speed;
      last_right = vibration->right_motor_speed;
    }
    return states_.count(id) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint16_t last_left = 0;
  uint16_t last_right = 0;

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE* out) override {
    if (states_.find(id) == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    auto it = keystrokes_.find(id);
    if (it == keystrokes_.end()) {
      return X_ERROR_EMPTY;
    }
    *out = it->second;
    return X_ERROR_SUCCESS;
  }

 private:
  bool keyboard_;
  std::vector<DeviceInfo> devices_;
  std::map<DeviceId, X_INPUT_STATE> states_;
  std::map<DeviceId, X_INPUT_KEYSTROKE> keystrokes_;
};

std::unique_ptr<InputSystem> SharedSystem(FakeDriver** out_driver) {
  auto driver = std::make_unique<FakeDriver>();
  *out_driver = driver.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(driver));
  system->SetDeviceAssignment(std::make_unique<SharedAssignment>());
  return system;
}

}  // namespace

TEST_CASE("SharedAssignment puts every device on user 0", "[input]") {
  SharedAssignment shared;
  shared.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});

  REQUIRE(For(shared, 0) ==
          std::vector<DeviceId>{static_cast<DeviceId>(10), static_cast<DeviceId>(11)});
  REQUIRE(For(shared, 1).empty());
}

TEST_CASE("SlotAssignment does not renumber survivors when a pad is unplugged", "[input]") {
  SlotAssignment slots;
  slots.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});
  slots.OnDevicesChanged({Pad(11, 1)});

  REQUIRE(For(slots, 0).empty());
  REQUIRE(For(slots, 1) == std::vector<DeviceId>{static_cast<DeviceId>(11)});
}

TEST_CASE("Both controllers reach one guest user", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B));
}

TEST_CASE("Unplugging one controller leaves the other working", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  driver->Remove(1);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("Rumble strength above 100 raises the title's motor values up to full speed", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  REQUIRE(rex::cvar::SetFlagByName("pad_rumble_strength", "200"));

  X_INPUT_VIBRATION vibration = {};
  vibration.left_motor_speed = 4915;
  vibration.right_motor_speed = 40000;
  REQUIRE(system->SetState(0, &vibration) == X_ERROR_SUCCESS);
  CHECK(driver->last_left == 9830);
  CHECK(driver->last_right == 65535);

  REQUIRE(rex::cvar::SetFlagByName("pad_rumble_strength", "50"));
  REQUIRE(system->SetState(0, &vibration) == X_ERROR_SUCCESS);
  CHECK(driver->last_left == 2457);
  CHECK(driver->last_right == 20000);

  rex::cvar::SetFlagByName("pad_rumble_strength", "100");
}

TEST_CASE("Capabilities follow the controller in hand", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);

  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(system->GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  REQUIRE(caps.sub_type == 2);
}

TEST_CASE("A physical controller answers capabilities before keyboard emulation", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  // Keyboard emulation enumerates first and nothing has been used yet.
  driver->Add(1, true);
  driver->Add(2);
  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(system->GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  CHECK(caps.sub_type == 2);

  // A key pressed last still leaves the controller answering.
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  CHECK(caps.sub_type == 2);
}

TEST_CASE("Keyboard emulation answers capabilities when it is alone", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1, true);
  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(system->GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  CHECK(caps.sub_type == 1);
}

TEST_CASE("A keystroke from the second controller reaches the guest", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->QueueKeystroke(2, 0x5800);

  X_INPUT_KEYSTROKE ks = {};
  REQUIRE(system->GetKeystroke(0, 0, &ks) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(ks.virtual_key) == 0x5800);
}

TEST_CASE("A reconnected pad comes back to guest user 0", "[input]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  driver->Add(1);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  // Replugging arrives as a new device id, not as the old one coming back.
  driver->Remove(1);
  driver->Add(2);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("A synthetic device does not push the first pad off guest user 0", "[input]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  // The NOP stand-in is always present and enumerates before any pad connects.
  driver->Add(1, true);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  driver->Add(2);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(1, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
}

namespace {

struct SlotSystem {
  FakeDriver* driver = nullptr;
  std::unique_ptr<InputSystem> system;

  SlotSystem() {
    auto owned = std::make_unique<FakeDriver>();
    driver = owned.get();
    system = std::make_unique<InputSystem>(nullptr);
    system->AddDriver(std::move(owned));
    system->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  }

  // Which device feeds `user`, through the capabilities stamp; 0 = none.
  uint8_t DeviceOf(uint32_t user) {
    X_INPUT_CAPABILITIES caps = {};
    if (system->GetCapabilities(user, 0, &caps) != X_ERROR_SUCCESS) {
      return 0;
    }
    return caps.sub_type;
  }
};

}  // namespace

TEST_CASE("A drum kit enumerated first leaves player 1 to the pad", "[input]") {
  SlotSystem slots;
  slots.driver->Add(1, false, true);
  slots.driver->Add(2);
  CHECK(slots.DeviceOf(0) == 2);
  CHECK(slots.DeviceOf(1) == 1);
}

TEST_CASE("A pad connected after a drum kit takes player 1 from it", "[input]") {
  SlotSystem slots;
  slots.driver->Add(1, false, true);
  CHECK(slots.DeviceOf(0) == 1);
  slots.driver->Add(2);
  CHECK(slots.DeviceOf(0) == 2);
  CHECK(slots.DeviceOf(1) == 1);
}

TEST_CASE("Two pads keep players 1 and 2 with a drum kit connected", "[input]") {
  SlotSystem slots;
  slots.driver->Add(1);
  slots.driver->Add(2, false, true);
  CHECK(slots.DeviceOf(0) == 1);
  CHECK(slots.DeviceOf(1) == 2);
  slots.driver->Add(3);
  CHECK(slots.DeviceOf(0) == 1);
  CHECK(slots.DeviceOf(1) == 3);
  CHECK(slots.DeviceOf(2) == 2);

  // The second pad leaves and comes back as a new device: still player 2.
  slots.driver->Remove(3);
  slots.driver->Add(4);
  CHECK(slots.DeviceOf(0) == 1);
  CHECK(slots.DeviceOf(1) == 4);
  CHECK(slots.DeviceOf(2) == 2);
}

TEST_CASE("A virtual pad leaves out the real pad it copies", "[input]") {
  using rex::input::PadFamily;
  using rex::input::sdl::HiddenBehindVirtualPad;
  // DS4Windows without exclusive mode: the real DS4 or DualSense goes.
  CHECK(HiddenBehindVirtualPad(PadFamily::kPlayStation, false, false, true));
  CHECK(HiddenBehindVirtualPad(PadFamily::kNintendo, false, false, true));
  // Its virtual DualShock 4 stays, and so do Xbox pads it does not copy.
  CHECK_FALSE(HiddenBehindVirtualPad(PadFamily::kPlayStation, true, false, true));
  CHECK_FALSE(HiddenBehindVirtualPad(PadFamily::kXbox, false, false, true));
  CHECK_FALSE(HiddenBehindVirtualPad(PadFamily::kUnknown, false, false, true));
  // Steam Input's virtual pad carries the real pad's family.
  CHECK_FALSE(HiddenBehindVirtualPad(PadFamily::kPlayStation, false, true, true));
  // No virtual pad: the real pad is used with its own features.
  CHECK_FALSE(HiddenBehindVirtualPad(PadFamily::kPlayStation, false, false, false));
}

namespace {

struct KeyboardAndPad {
  FakeDriver* keyboard = nullptr;
  FakeDriver* pad = nullptr;
  std::unique_ptr<InputSystem> system;
};

KeyboardAndPad KeyboardAndPadSystem() {
  KeyboardAndPad out;
  auto keyboard = std::make_unique<FakeDriver>(true);
  auto pad = std::make_unique<FakeDriver>();
  out.keyboard = keyboard.get();
  out.pad = pad.get();
  out.keyboard->Add(1, true);
  out.pad->Add(2);
  out.system = std::make_unique<InputSystem>(nullptr);
  out.system->AddDriver(std::move(keyboard));
  out.system->AddDriver(std::move(pad));
  out.system->SetDeviceAssignment(std::make_unique<SharedAssignment>());
  return out;
}

}  // namespace

TEST_CASE("The keyboard state filter sees only the keyboard", "[input][fm4_mnk_bind]") {
  auto setup = KeyboardAndPadSystem();
  setup.keyboard->SetButtons(1, X_INPUT_GAMEPAD_A);
  setup.pad->SetButtons(2, X_INPUT_GAMEPAD_B);
  int calls = 0;
  setup.system->SetKeyboardStateFilter([&](uint32_t user, rex::input::X_INPUT_GAMEPAD& pad) {
    ++calls;
    CHECK(user == 0);
    CHECK(static_cast<uint16_t>(pad.buttons) == X_INPUT_GAMEPAD_A);
    pad.right_trigger = 0x80;
  });
  X_INPUT_STATE state = {};
  REQUIRE(setup.system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(calls == 1);
  CHECK(static_cast<uint16_t>(state.gamepad.buttons) == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B));
  CHECK(state.gamepad.right_trigger == 0x80);
}

TEST_CASE("The active device tracker sees the keyboard before the filter",
          "[input][fm4_mnk_bind]") {
  auto setup = KeyboardAndPadSystem();
  setup.keyboard->SetButtons(1, X_INPUT_GAMEPAD_A);
  setup.system->SetKeyboardStateFilter(
      [](uint32_t, rex::input::X_INPUT_GAMEPAD& pad) { pad = {}; });
  X_INPUT_STATE state = {};
  REQUIRE(setup.system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint16_t>(state.gamepad.buttons) == 0);
  const auto active = setup.system->ActiveDevice(0);
  REQUIRE(active);
  CHECK(active->id == static_cast<DeviceId>(1));
}
