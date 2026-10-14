#pragma once
#include <windows.h>
#include <string>

// Self-installation: FreeDisplay.exe carries the tablet app and adb as resources
// and can install itself per user (no admin) into %LOCALAPPDATA%\Programs\FreeDisplay.
namespace installer {

// Message the tray window accepts as "quit now" (used to stop a running copy).
constexpr UINT kQuitMsg = WM_APP + 3;

std::wstring InstallDir(); // with trailing backslash

// Directory holding FreeDisplay.apk and adb\ for this run (with trailing backslash).
const std::wstring& PayloadDir();

// True when FreeDisplay.apk sits next to the executable: an installed copy or a
// development build. Such a copy runs in place without offering to install.
bool RunningInPlace();

// Writes the embedded files into `dir` (skipping ones already identical).
bool ExtractPayload(const std::wstring& dir);

// For a copy that is not running in place: extracts the payload to a per-user
// cache so the app can run without installing, and points PayloadDir() there.
bool PreparePortable();

// Installs the running executable for the current user and starts the installed
// copy unless `launch` is false. Shows errors unless `quiet`.
bool Install(bool quiet, bool launch);

// Removes everything Install() created. Asks first unless `quiet`.
bool Uninstall(bool quiet);

// Asks a running FreeDisplay to quit and waits for it. Returns false if it would not stop.
bool StopRunningInstance(DWORD timeoutMs = 10000);

} // namespace installer
