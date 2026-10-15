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

1. Run `FreeDisplay.exe` and choose **Yes**. It installs for your user into
   `%LOCALAPPDATA%\Programs\FreeDisplay`, adds a Start menu shortcut and an
   Apps & Features entry, and starts with Windows. Choose **No** to run it once
   without installing.
2. If [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
   (which creates the extra screen) is missing, FreeDisplay offers to install
   the copy it includes. Windows asks for administrator permission once, then
   you restart the PC. Without the driver, FreeDisplay mirrors the main screen.
3. FreeDisplay now runs in the notification area (tray).

The included driver gets a settings file (`C:\VirtualDisplayDriver\vdd_settings.xml`)
listing common tablet resolutions at 60-144 Hz. If your tablet's resolution is
not listed, add it there and restart. A driver you installed yourself is used
as it is.

Silent options: `FreeDisplay.exe --install --quiet` (skips the driver),
`FreeDisplay.exe --uninstall --quiet`. Uninstall normally from
**Settings > Apps > Installed apps > FreeDisplay**; it offers to remove the
driver too if FreeDisplay installed it.

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
- **Screen mode**: Extend (tablet is a second screen) or Duplicate (mirror),
  the same as Win+P; the stream follows within a second or two, also when you
  use Win+P.
- **Start with Windows**, **Open log**, **Restart app**.

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

Official releases sign the tablet app with the project's release key, which is
kept outside this repository. To sign your own builds, set the Gradle property
`freedisplay.signing` (for example in `%USERPROFILE%\.gradle\gradle.properties`)
to a properties file with `storeFile`, `storePassword`, `keyAlias` and
`keyPassword`; without it, builds use your local debug key. FreeDisplay replaces
a tablet app signed with a different key automatically.

## Encoder selection

`auto` picks by the GPU that renders the captured monitor: AMD -> AMF
(ultra-low-latency), NVIDIA -> NVENC (ultra-low-latency), anything else (or if
those fail) -> Media Foundation.

## Tools

- `host/tools/bench.ps1`: latency benchmark (needs `animate.exe`).
- `host/tools/traytest.ps1`: end-to-end test of the tray menu with adb swipes.
- `host/tools/nvenctest.cpp`: NVENC speed test without changing displays.
- `host/tools/touchtest.cpp`, `pentest.cpp`: input-injection diagnostics.

## License

FreeDisplay is MIT licensed (see [LICENSE](LICENSE)). It is an independent
project written from scratch: it contains no code from, and is not affiliated
with, any commercial second-screen app. It is provided as is, without warranty.

## Third-party

- `host/third_party/amf`: AMD AMF SDK headers (MIT, see its LICENSE.txt).
- `host/third_party/nvenc`: NVENC API headers from FFmpeg nv-codec-headers (MIT).
- `FreeDisplay.exe` bundles `adb` from the Android SDK Platform-Tools (Apache
  License 2.0, part of the Android Open Source Project); its NOTICE.txt is
  installed next to it in `adb\`.
- `host/third_party/vdd`: the unmodified, signed Virtual Display Driver package
  (MttVDD 23.40.36.27) by VirtualDrivers / MikeTheTech, MIT license (see its
  LICENSE.txt, also installed next to the driver). FreeDisplay installs it only
  when no copy is present.
