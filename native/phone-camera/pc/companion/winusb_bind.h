#pragma once

#include <string>

namespace mycam {

// True for a device instance ID (as Device Manager shows it) that is a phone's accessory interface: the whole
// device in plain accessory mode, or interface 0 of the composite device USB debugging makes.
bool IsAccessoryInstance(const std::wstring& id);

// True if a phone in accessory mode is attached with no driver (Windows has none for 18d1:2d0x).
bool AccessoryNeedsDriver();

// Binds Windows' inbox WinUSB driver to every driverless accessory-mode phone. Requires admin.
// Returns how many devices were bound. The binding persists for that phone.
int BindAccessoryDrivers();

} // namespace mycam
