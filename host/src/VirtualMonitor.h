#pragma once
#include <windows.h>
#include <string>

// Shows the Virtual Display Driver monitor only while a tablet is streaming.
// Detaching/attaching goes through ChangeDisplaySettingsEx (no admin needed);
// Windows moves windows off a monitor when it is detached.
namespace VirtualMonitor {

// GDI name (\\.\DISPLAYn) of the VDD monitor, attached or not; empty if VDD is absent.
std::wstring Find();

bool IsAttached();

// Attaches the monitor at width x height @ hz (falling back to its last mode)
// at its last position, or left of the primary monitor the first time.
bool Attach(UINT width, UINT height, UINT hz);

// Remembers the monitor's position, then detaches it.
bool Detach();

} // namespace VirtualMonitor
