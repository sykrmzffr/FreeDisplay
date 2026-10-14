# FreeDisplay

Use an Android tablet as a second monitor for Windows over a USB cable, with
touch and pen input. Free and built from our own code.

## How it works

```
Windows (host/)                                   Tablet (android/)
Virtual Display Driver monitor
  -> DXGI Desktop Duplication + GPU NV12 convert
  -> H.264: AMD AMF / NVIDIA NVENC / Media Foundation  --USB (adb reverse)-->  MediaCodec low-latency decode
<- touch (InjectTouchInput), pen (as mouse)        <-- touch/stylus events --  full-screen view at 120 Hz
```

The host detects the tablet, installs/updates the app over adb, launches it
and starts streaming. Wire format: [PROTOCOL.md](PROTOCOL.md).

## Requirements

- Windows 10/11 with [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
  installed and enabled. Set its `vdd_settings.xml` GPU to the one whose
  encoder you want to use, and add your tablet's resolution.
- Android 8+ tablet with **USB debugging** enabled (one-time setup).
- To build: Visual Studio 2022 C++ build tools, Android SDK, JDK 17+.

## Build

```bat
host\build.bat
```

```bat
cd android
gradle assembleRelease
copy app\build\outputs\apk\release\app-release.apk ..\host\out\FreeDisplay.apk
```

`FreeDisplay.apk` must sit next to `FreeDisplayHost.exe`; the host installs it.

## Use

Run `host\out\FreeDisplayHost.exe` and plug in the tablet. The host lives in the
notification area; its menu has:

- **Touch input / Pen input**: turn tablet input on or off (applies instantly).
- **Video quality**: Sharp (full resolution), Balanced (75%, default),
  Fastest (50%). Lower = less latency, softer text.
- **Start with Windows**, **Open log**, **Quit**.

Settings: `%APPDATA%\FreeDisplay\settings.ini`. Log: `%APPDATA%\FreeDisplay\host.log`.

Command-line options for testing: `--console`, `--list`, `--display N`,
`--fps N`, `--bitrate Mbps`, `--scale PERCENT`, `--encoder auto|amf|nvenc|mf`,
`--no-install`.

## Encoder selection

`auto` picks by the GPU that renders the captured monitor: AMD -> AMF
(ultra-low-latency), NVIDIA -> NVENC (ultra-low-latency), anything else (or if
those fail) -> Media Foundation.

## Tools

- `host/tools/bench.ps1`: latency benchmark (needs `animate.exe`).
- `host/tools/traytest.ps1`: end-to-end test of the tray menu with adb swipes.
- `host/tools/nvenctest.cpp`: NVENC speed test without changing displays.
- `host/tools/touchtest.cpp`, `pentest.cpp`: input-injection diagnostics.

## Third-party

- `host/third_party/amf`: AMD AMF SDK headers (MIT).
- `host/third_party/nvenc`: NVENC API headers from FFmpeg nv-codec-headers (MIT).
