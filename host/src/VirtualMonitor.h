#pragma once
#include <windows.h>
#include <string>

// Helpers for the Virtual Display Driver (VDD) monitor.
//
// The monitor is left attached at all times. Attaching/detaching it through
// registry-backed display changes (CDS_UPDATEREGISTRY) hung VDD and froze the
// whole system in testing, so only the in-place mode switch below is used, and
// only before capture starts.
// Disabling/re-enabling the VDD device works, but capturing its monitor afterwards
// hung the graphics stack too, so when the tablet goes away its windows are moved
// back to the main screen instead.
namespace VirtualMonitor {

// GDI name (\\.\DISPLAYn) of the VDD monitor; empty if VDD is not installed.
std::wstring Find();

// True when the VDD monitor is part of the desktop.
bool IsAttached();

// Switches the VDD monitor to the offered mode that best fits a tablet of
// tabletW x tabletH (same orientation, closest shape, closest size) at the refresh
// rate closest to `hz`. Uses a non-persistent ChangeDisplaySettingsEx call (no
// registry update). Returns true when the mode was changed.
bool MatchTablet(const std::wstring& gdiName, UINT tabletW, UINT tabletH, UINT hz);

// True when the VDD monitor offers w x h (in that orientation).
bool HasMode(const std::wstring& gdiName, UINT w, UINT h);

// Projection as in Win+P: 0 Extend, 1 Duplicate, 2 PC screen only, -1 other.
int Topology();

// Same as picking that Win+P option (done by Windows, which keeps the VDD device
// running; only the monitor leaves the desktop). True on success.
bool SetTopology(int mode);

// True when the active screens are built-in panel(s) plus the VDD monitor and
// nothing else, so "PC screen only" turns off just the tablet's screen.
bool OnlyVddBesidesBuiltIn();

// Moves app windows (and the pointer) off the VDD monitor onto the primary
// monitor, keeping their state (normal/maximized/minimized) and their place
// relative to the screen. Returns the number of windows moved.
int MoveWindowsToPrimary();

} // namespace VirtualMonitor
