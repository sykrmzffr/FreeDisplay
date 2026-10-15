#pragma once
#include <windows.h>
#include <string>

// Helpers for the Virtual Display Driver (VDD) monitor.
//
// The monitor is left attached at all times. Attaching/detaching it through
// registry-backed display changes (CDS_UPDATEREGISTRY) hung VDD and froze the
// whole system in testing, so only the in-place refresh-rate switch below is used.
// Disabling/re-enabling the VDD device works, but capturing its monitor afterwards
// hung the graphics stack too, so when the tablet goes away its windows are moved
// back to the main screen instead.
namespace VirtualMonitor {

// GDI name (\\.\DISPLAYn) of the VDD monitor; empty if VDD is not installed.
std::wstring Find();

// True when the VDD monitor is part of the desktop.
bool IsAttached();

// Switches an attached display to `hz` at its current resolution, if that mode
// exists. Uses a non-persistent ChangeDisplaySettingsEx call (no registry update).
void SetRefreshRate(const std::wstring& gdiName, UINT hz);

// Moves app windows (and the pointer) off the VDD monitor onto the primary
// monitor, keeping their state (normal/maximized/minimized) and their place
// relative to the screen. Returns the number of windows moved.
int MoveWindowsToPrimary();

} // namespace VirtualMonitor
