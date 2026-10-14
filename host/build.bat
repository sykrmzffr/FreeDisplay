@echo off
rem Builds FreeDisplay.exe with the Visual Studio 2022 C++ build tools.
rem The tablet app (android\app\build\outputs\apk\release\app-release.apk, see
rem build-all.ps1) and adb from the Android SDK platform-tools are embedded in it.
setlocal
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
  echo Visual Studio C++ build tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1

cd /d "%~dp0"
if not exist out\payload mkdir out\payload

rem ---- payload: tablet app + adb ----
set APK=..\android\app\build\outputs\apk\release\app-release.apk
if exist "%APK%" copy /y "%APK%" out\payload\FreeDisplay.apk >nul
if not exist out\payload\FreeDisplay.apk (
  echo Tablet app not built. Run build-all.ps1, or build android\ with "gradle assembleRelease".
  exit /b 1
)
rem A copy next to the exe makes out\FreeDisplay.exe run in place (no install prompt).
copy /y out\payload\FreeDisplay.apk out\FreeDisplay.apk >nul

set PT=
if defined ANDROID_HOME if exist "%ANDROID_HOME%\platform-tools\adb.exe" set PT=%ANDROID_HOME%\platform-tools
if not defined PT if exist "%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe" set PT=%LOCALAPPDATA%\Android\Sdk\platform-tools
if not defined PT (
  echo Android SDK platform-tools not found. Set ANDROID_HOME.
  exit /b 1
)
for %%f in (adb.exe AdbWinApi.dll AdbWinUsbApi.dll NOTICE.txt) do copy /y "%PT%\%%f" out\payload\ >nul || exit /b 1

rem ---- resources + code ----
rc /nologo /fo out\FreeDisplay.res res\FreeDisplay.rc || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W3 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
  /I third_party /Fo:out\ /Fe:out\FreeDisplay.exe ^
  src\main.cpp src\Adb.cpp src\Capture.cpp src\Encoder.cpp src\AmfEncoder.cpp src\NvencEncoder.cpp src\Input.cpp src\Installer.cpp src\Settings.cpp src\Tray.cpp src\VirtualMonitor.cpp ^
  out\FreeDisplay.res ^
  /link /SUBSYSTEM:WINDOWS /MANIFEST:NO d3d11.lib dxgi.lib mfplat.lib mf.lib mfuuid.lib ws2_32.lib user32.lib ole32.lib oleaut32.lib winmm.lib strmiids.lib shell32.lib gdi32.lib advapi32.lib comctl32.lib
exit /b %errorlevel%
