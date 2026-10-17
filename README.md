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

- A Windows 11 PC, 64-bit (Windows 10 version 2004 or newer should work but is
  untested). Any GPU: AMD and NVIDIA use their own encoders, Intel and others
  use Windows' Media Foundation encoder.
- An Android tablet or phone with Android 8.0 or newer, any size, shape or
  resolution.
- A USB cable that carries data (some charging-only cables do not).

## Quick start (download from GitHub)

1. Open the [latest release](https://github.com/sykrmzffr/FreeDisplay/releases/latest)
   and download **FreeDisplay.exe**. (The **FreeDisplay.apk** there is only needed
   to install the tablet app by hand; the PC app installs it for you.)
2. Run `FreeDisplay.exe`. Windows SmartScreen may say it "protected your PC"
   because the program is new and unsigned: click **More info > Run anyway**.
3. Choose **Yes** to install. When it offers the extra-screen driver, choose
   **Yes** and allow the administrator prompt, then restart the PC when asked.
4. On the tablet, turn on USB debugging (see [Set up the tablet](#set-up-the-tablet-once)).
5. Plug the tablet in and tap **Allow** on its USB debugging prompt. FreeDisplay
   installs and opens its tablet app, and the tablet becomes a second screen.
6. Arrange the screen in **Windows Settings > System > Display**, and click the
   FreeDisplay tray icon for settings.

The first time you use a tablet whose resolution FreeDisplay has not seen, the
picture is scaled to fit and FreeDisplay asks you to restart Windows once; after
that the screen matches the tablet pixel for pixel.

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
with common screen shapes (16:9, 20:9, 16:10, 4:3, 3:2) in landscape and portrait
at 60 and 120 Hz. When a tablet's exact resolution is missing, FreeDisplay adds
it (keeping the last two tablets' sizes; the driver only handles about 48 modes)
and asks for one restart; until then it scales the closest shape. A driver you
installed yourself, with your own settings file, is used as it is.

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
copy `FreeDisplay.apk` (from the release, or `dist\tablet\` when building) to
the tablet and open it.

If the tablet is not detected: try another cable or USB port, make sure the
prompt on the tablet was allowed, and set the USB mode to **File transfer**.
Some brands (e.g. Samsung) need their USB driver on Windows for USB debugging.

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
- **Turn off the tablet screen when it disconnects** (on by default): when the
  cable is unplugged, or the tablet app is closed for 3 seconds, FreeDisplay
  switches Windows to "PC screen only" (as Win+P does), so the extra screen is
  gone and windows and the pointer stay on the main screen. Plugging the tablet
  back in restores Extend (or Duplicate). With another monitor attached it only
  moves windows back, so that monitor stays on. The driver itself stays
  installed and running: switching it off and on froze Windows in testing.
- **Orientation**: Auto (follows the tablet), Landscape or Portrait. The
  virtual monitor switches to the matching shape. Android 16 lets large
  tablets ignore an app's rotation request, so when the tablet's screen is the
  other way round, the tablet app turns the picture itself; turn the tablet
  to read it, and touch and pen follow.
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
