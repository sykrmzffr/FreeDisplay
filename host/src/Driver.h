#pragma once
#include <windows.h>

// Installs and removes the Virtual Display Driver (MttVDD), which provides the
// extra monitor. FreeDisplay.exe carries the unmodified, signed driver package
// (host/third_party/vdd). Installing needs admin rights, so the elevated parts
// run in a second copy of this executable started with a UAC prompt.
//
// After an install Windows must be restarted before the new monitor is captured:
// capturing a VDD monitor whose driver was started while Windows was running
// froze the test laptop, while after a fresh boot it always worked.
namespace driver {

// True when a VDD device exists (installed by us or by its own installer).
bool Present();

// True when FreeDisplay installed the driver and Windows has not restarted since.
bool RestartPending();

// True when the driver was installed by FreeDisplay (so uninstalling may remove it).
bool InstalledByUs();

// Start an elevated copy (UAC prompt) and wait for it. False if declined or failed.
bool RunElevatedInstall();
bool RunElevatedUninstall();

// Elevated entry points (--install-driver / --uninstall-driver). Return exit codes.
int Install();
int Uninstall();

// Asks Windows to restart. Returns false if it refused.
bool RestartWindows();

} // namespace driver
