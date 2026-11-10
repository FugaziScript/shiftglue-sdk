/**
 * ViGEmBus virtual pads, found through the device tree: SDL reports a virtual
 * pad like any real one.
 */

#include <rex/input/sdl/virtual_pad.h>

#include <windows.h>
#include <initguid.h>
#include <cfgmgr32.h>
#include <devpkey.h>

#include <cwchar>
#include <type_traits>
#include <vector>

namespace rex::input::sdl {

namespace {

constexpr wchar_t kBusService[] = L"ViGEmBus";

// cfgmgr32.dll is loaded on demand, as hid.dll is for the direct reports.
struct ConfigManager {
  decltype(&CM_Get_Device_ID_List_SizeW) list_size = nullptr;
  decltype(&CM_Get_Device_ID_ListW) list = nullptr;
  decltype(&CM_Locate_DevNodeW) locate = nullptr;
  decltype(&CM_Get_Child) child = nullptr;
  decltype(&CM_Get_Parent) parent = nullptr;
  decltype(&CM_Get_DevNode_Registry_PropertyW) registry_property = nullptr;
  decltype(&CM_Get_Device_Interface_PropertyW) interface_property = nullptr;
  bool loaded = false;
};

const ConfigManager& Cm() {
  static const ConfigManager cm = [] {
    ConfigManager out;
    HMODULE module = LoadLibraryW(L"cfgmgr32.dll");
    if (!module) {
      return out;
    }
    auto load = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, name));
      return fn != nullptr;
    };
    out.loaded = load(out.list_size, "CM_Get_Device_ID_List_SizeW") &&
                 load(out.list, "CM_Get_Device_ID_ListW") &&
                 load(out.locate, "CM_Locate_DevNodeW") && load(out.child, "CM_Get_Child") &&
                 load(out.parent, "CM_Get_Parent") &&
                 load(out.registry_property, "CM_Get_DevNode_Registry_PropertyW") &&
                 load(out.interface_property, "CM_Get_Device_Interface_PropertyW");
    return out;
  }();
  return cm;
}

bool IsBus(const ConfigManager& cm, DEVINST node) {
  wchar_t service[64] = {};
  ULONG size = sizeof(service);
  ULONG type = 0;
  return cm.registry_property(node, CM_DRP_SERVICE, &type, service, &size, 0) == CR_SUCCESS &&
         _wcsicmp(service, kBusService) == 0;
}

}  // namespace

bool VirtualPadPresent() {
  const ConfigManager& cm = Cm();
  if (!cm.loaded) {
    return false;
  }
  const ULONG flags = CM_GETIDLIST_FILTER_SERVICE | CM_GETIDLIST_FILTER_PRESENT;
  ULONG length = 0;
  if (cm.list_size(&length, kBusService, flags) != CR_SUCCESS || length <= 1) {
    return false;
  }
  std::vector<wchar_t> ids(length);
  if (cm.list(kBusService, ids.data(), length, flags) != CR_SUCCESS) {
    return false;
  }
  for (wchar_t* id = ids.data(); *id; id += std::wcslen(id) + 1) {
    DEVINST bus = 0;
    DEVINST pad = 0;
    if (cm.locate(&bus, id, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
        cm.child(&pad, bus, 0) == CR_SUCCESS) {
      return true;
    }
  }
  return false;
}

bool IsVirtualPadPath(const std::string& path) {
  const ConfigManager& cm = Cm();
  if (!cm.loaded || path.rfind("\\\\?\\", 0) != 0) {
    return false;
  }
  // HID device paths are ASCII.
  const std::wstring interface_path(path.begin(), path.end());
  wchar_t instance[MAX_DEVICE_ID_LEN] = {};
  ULONG size = sizeof(instance);
  DEVPROPTYPE type = 0;
  if (cm.interface_property(interface_path.c_str(), &DEVPKEY_Device_InstanceId, &type,
                            reinterpret_cast<PBYTE>(instance), &size, 0) != CR_SUCCESS) {
    return false;
  }
  DEVINST node = 0;
  if (cm.locate(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
    return false;
  }
  while (cm.parent(&node, node, 0) == CR_SUCCESS) {
    if (IsBus(cm, node)) {
      return true;
    }
  }
  return false;
}

}  // namespace rex::input::sdl
