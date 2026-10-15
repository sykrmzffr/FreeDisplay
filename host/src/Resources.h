#pragma once
// Resource IDs shared by FreeDisplay.rc and the code. Keep the version in sync
// with VERSIONINFO in res/FreeDisplay.rc and versionName in the Android app.

#define FD_VERSION_STR "1.3.0"
#define FD_VERSION_WSTR L"1.3.0"

#define IDI_APP 1

// Files embedded in the executable and written out on install.
#define IDR_APK 101
#define IDR_ADB 102
#define IDR_ADBWINAPI 103
#define IDR_ADBWINUSBAPI 104
#define IDR_ADB_NOTICE 105

// Virtual Display Driver package (host/third_party/vdd), installed on demand.
#define IDR_VDD_INF 110
#define IDR_VDD_DLL 111
#define IDR_VDD_CAT 112
#define IDR_VDD_LICENSE 113
