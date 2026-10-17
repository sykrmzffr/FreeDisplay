# Changelog

All notable changes to FreeDisplay. Versions follow [Semantic Versioning](https://semver.org/).

## [1.3.0] - 2026-10-17

### Added
- **Portrait mode.** New **Orientation** setting (Auto, Landscape, Portrait) in
  the settings window and tray menu. The virtual screen switches to the
  tablet's shape, portrait included.
- On tablets that ignore an app's rotation request (Android 16 on large
  screens), the tablet app turns the picture itself and maps touch and pen to
  match; a motion sensor picks the side you turn the tablet to.
- **Works with any tablet or phone shape.** The virtual screen offers common
  shapes (16:9, 20:9, 16:10, 4:3, 3:2) and learns the exact resolution of the
  devices you use; after one restart the screen fits a new device pixel for pixel.
- **The extra screen turns off when the tablet is unplugged** (or its app is
  closed), like choosing "PC screen only" in Win+P, so the pointer and windows
  can no longer end up on a screen that isn't there. Plugging the tablet back
  in restores Extend or Duplicate. With another monitor attached, windows are
  moved back instead, so that monitor stays on.
- README: requirements, quick start from GitHub, troubleshooting.

### Changed
- The tablet reports the largest picture its decoder supports at its own
  shape, so devices with stricter decoder limits no longer get a stream they
  can't play.
- "Move windows back when the tablet disconnects" is now "Turn off the tablet
  screen when it disconnects" (same setting, on by default).

### Upgrading from 1.2.0
- Run the new `FreeDisplay.exe`; the tablet app updates itself on the next
  connection. The new screen shapes take effect after your next restart.

## [1.2.0] - 2026-10-15

### Added
- FreeDisplay installs the Virtual Display Driver for you when it is missing
  (one administrator prompt, then a restart). Uninstalling offers to remove it.
- **Screen mode** (Extend / Duplicate) and a **Restart app** button in the
  settings window and tray menu.

### Fixed
- The virtual screen was missing on some PCs because the driver was given too
  many display modes; it also rendered on the wrong GPU on hybrid laptops.
- Building failed when PATH contained folders with parentheses.

## [1.1.2] - 2026-10-15

### Added
- MIT license and third-party notices.
- The tablet app is signed with a permanent release key. An app installed from
  an older build is replaced automatically.

## [1.1.1] - 2026-10-15

### Fixed
- Switching Win+P between Extend and Duplicate while connected now takes effect
  within a second, without replugging.
- A crash after the display layout changed.
- In Duplicate mode the laptop screen's refresh rate is no longer changed.

## [1.1.0] - 2026-10-15

### Added
- **Battery saver:** 60 Hz instead of up to 120 Hz, when on battery (default),
  always, or off.
- **Settings window:** click the tray icon or start FreeDisplay again.
- Windows on the tablet screen and the pointer move back to the main screen
  when the tablet disconnects.

## [1.0.0] - 2026-10-15

First release.

- Use an Android tablet as a second screen for Windows over USB, with touch
  and pen input and up to 120 Hz.
- Low-latency H.264 streaming with AMD AMF, NVIDIA NVENC or Windows Media
  Foundation; low-latency hardware decoding on the tablet.
- One self-installing `FreeDisplay.exe`: installs per user without admin
  rights, starts with Windows, installs and updates the tablet app by itself.
- Tray menu: touch and pen switches, video quality presets, start with Windows.

[1.3.0]: https://github.com/sykrmzffr/FreeDisplay/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/sykrmzffr/FreeDisplay/compare/v1.1.2...v1.2.0
[1.1.2]: https://github.com/sykrmzffr/FreeDisplay/compare/v1.1.1...v1.1.2
[1.1.1]: https://github.com/sykrmzffr/FreeDisplay/releases/tag/v1.1.1
[1.1.0]: https://github.com/sykrmzffr/FreeDisplay/commits/main
[1.0.0]: https://github.com/sykrmzffr/FreeDisplay/commits/main
