#pragma once

#include <string>

#include <rex/input/device.h>

namespace rex::input::sdl {

/// DS4Windows and similar tools copy a PlayStation or Nintendo pad into a
/// virtual Xbox 360 or DualShock 4 pad on the ViGEmBus driver. Without their
/// exclusive mode the game sees both and every press arrives twice, so the
/// real pad is left out and the virtual one used: a single program then
/// drives the pad, as in exclusive mode.
inline bool HiddenBehindVirtualPad(PadFamily family, bool is_virtual, bool steam_virtual,
                                   bool virtual_present) {
  return virtual_present && !is_virtual && !steam_virtual &&
         (family == PadFamily::kPlayStation || family == PadFamily::kNintendo);
}

/// Windows only (virtual_pad_win.cpp); callers guard them with _WIN32.
/// A ViGEmBus device has a virtual pad plugged in.
bool VirtualPadPresent();
/// The HID device at `path` (a `\\?\HID#...` interface path, as SDL's HIDAPI
/// and RAWINPUT drivers report it) is a ViGEmBus virtual pad.
bool IsVirtualPadPath(const std::string& path);

}  // namespace rex::input::sdl
