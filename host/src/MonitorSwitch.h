#pragma once
#include <windows.h>

// Turns the Virtual Display Driver device off while no tablet is connected, so
// Windows sees no extra monitor (windows and the pointer stay on real screens).
//
// Disabling a device needs admin rights. A one-time elevated setup registers two
// on-demand scheduled tasks, run as SYSTEM, that only call Windows' own
// pnputil.exe with fixed arguments for the VDD device. The tasks' security
// descriptor lets the installing user start them, so later switches need no prompt.
// This replaces display-settings attach/detach, which hung VDD in testing.
namespace MonitorSwitch {

// True when the Virtual Display Driver device is installed (enabled or not).
bool VddInstalled();

// True when the scheduled tasks exist (setup was done).
bool Available();

// True when the VDD device is enabled (its monitor exists).
bool IsOn();

// True after a switch went wrong. Switching then stays paused until restart and
// the monitor is left on.
bool Broken();

// Held while a monitor is being captured. Set() refuses to switch meanwhile.
struct CaptureGuard {
    CaptureGuard();
    ~CaptureGuard();
    CaptureGuard(const CaptureGuard&) = delete;
    CaptureGuard& operator=(const CaptureGuard&) = delete;
};

// Enables or disables the VDD device and waits for the monitor to appear or
// disappear (up to `timeoutMs`). Switches are spaced at least 5 s apart, never
// happen during a capture, and a monitor that appears gets 2 s to settle.
// Returns true when the monitor reached the state.
bool Set(bool on, DWORD timeoutMs = 10000);

// Runs this executable elevated (UAC prompt) to register the tasks. Blocks until done.
bool RunElevatedSetup();

// Runs elevated to turn the monitor back on and delete the tasks (for uninstall).
bool RunElevatedRemoval();

// Elevated entry points (--setup-monitor-switch / --remove-monitor-switch).
int SetupTasks();
int RemoveTasks();

} // namespace MonitorSwitch
