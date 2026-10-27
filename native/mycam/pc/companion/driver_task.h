#pragma once

namespace mycam {

// A Task Scheduler task, registered by the (elevated) installer, that runs "MyCamCompanion.exe --bind-driver"
// as SYSTEM. Signed-in users may start it but not change it, so a new phone gets its WinUSB driver without
// a UAC prompt. The task only ever binds Windows' own WinUSB driver to accessory-mode phones.

// Creates or replaces the task for the running exe. Requires admin. Returns false on failure.
bool RegisterDriverTask();

// Starts the task. Returns false if it isn't registered or can't be run (the caller then falls back to UAC).
bool RunDriverTask();

} // namespace mycam
