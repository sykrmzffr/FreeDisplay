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

## Install (Windows)

1. Install [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
   (this provides the extra monitor). In its `vdd_settings.xml`, pick the GPU
   whose video encoder you want to use and add your tablet's resolution.
2. Run `FreeDisplay.exe` (from `dist\` after building) and choose **Yes**.
   It installs for your user only (no admin rights) into
   `%LOCALAPPDATA%\Programs\FreeDisplay`. It also adds a Start menu shortcut and
   an Apps & Features entry, and it starts with Windows. Choose **No** to run it once without installing.
3. FreeDisplay now runs in the notification area (tray).

Silent options: `FreeDisplay.exe --install --quiet`, `FreeDisplay.exe --uninstall --quiet`.
Uninstall normally from **Settings > Apps > Installed apps > FreeDisplay**.

## Set up the tablet (once)

1. **Settings > About tablet**: tap **OS version** (Xiaomi) or **Build number**
   (most others) 7 times to unlock Developer options.
2. **Developer options**: turn on **USB debugging**. On Xiaomi, also turn on
   **USB debugging (Security settings)** and **Install via USB** so input and
   automatic app updates work.
3. Plug the tablet into the PC and tap **Allow** on the USB debugging prompt
   (tick "Always allow from this computer").

FreeDisplay then installs its tablet app by itself and opens it. After that,
plugging in the cable is all it takes. To install the tablet app by hand,
copy `dist\tablet\FreeDisplay.apk` to the tablet and open it.

## Use

Click the tray icon (or start FreeDisplay again from the Start menu) for the
settings window; right-click it for the same options as a menu, plus **Quit**:

- **Touch input / Pen input**: turn tablet input on or off (applies instantly).
- **Video quality**: Sharp (full resolution), Balanced (75%, default),
  Fastest (50%). Lower = less latency, softer text.
- **Battery saver**: When on battery (default), Always, or Off. It runs the
  virtual monitor, the stream and the tablet screen at 60 Hz instead of up to
  120 Hz. Turning it on or off (or plugging in the charger) restarts the
  stream for a moment, so the refresh rate is only changed before capture starts.
  On Xiaomi tablets, set **Settings > Display > Refresh rate** to 120 Hz, or
  the system limits apps to 90 Hz.
- **Move windows back when the tablet disconnects** (on by default): when the
  cable is unplugged, or the tablet app is closed for 3 seconds, windows on the
  tablet screen and the mouse pointer return to the main screen. The virtual
  monitor itself stays on: switching the driver off and on worked, but
  capturing it afterwards froze Windows in testing.
- **Start with Windows**, **Open log**.

`FreeDisplay.exe --move-windows-back` does the window move by hand.

Arrange the extra monitor in **Windows Settings > System > Display** like any monitor.

Settings: `%APPDATA%\FreeDisplay\settings.ini`. Log: `%APPDATA%\FreeDisplay\host.log`.

Command-line options for testing: `--console`, `--list`, `--display N`,
`--fps N`, `--bitrate Mbps`, `--scale PERCENT`, `--encoder auto|amf|nvenc|mf`,
`--no-install`.

## Build

Needs Visual Studio 2022 C++ build tools, the Android SDK and Android Studio
(or JDK 17+ and Gradle).

```powershell
.\build-all.ps1
```

This builds the tablet app and then `host\out\FreeDisplay.exe`, which has the
tablet app and adb (from SDK platform-tools) embedded. The result is copied to
`dist\FreeDisplay.exe`; `dist\tablet\FreeDisplay.apk` is for manual installs.
`host\out\FreeDisplay.exe` runs in place without offering to install, because
`FreeDisplay.apk` sits next to it.

The tablet app is signed with this PC's Android debug key. If you build on
another PC, uninstall the tablet app once so the new build can be installed.

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
