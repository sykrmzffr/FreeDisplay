@echo off
rem Builds FreeDisplayHost.exe with the Visual Studio 2022 C++ build tools.
setlocal
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
  echo Visual Studio C++ build tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /std:c++20 /EHsc /O2 /W3 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
  /I third_party /Fo:out\ /Fe:out\FreeDisplayHost.exe ^
  src\main.cpp src\Adb.cpp src\Capture.cpp src\Encoder.cpp src\AmfEncoder.cpp src\NvencEncoder.cpp src\Input.cpp ^
  /link d3d11.lib dxgi.lib mfplat.lib mf.lib mfuuid.lib ws2_32.lib user32.lib ole32.lib oleaut32.lib winmm.lib strmiids.lib
exit /b %errorlevel%
