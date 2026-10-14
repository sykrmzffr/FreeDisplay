// Lists display modes for a GDI display name, e.g. modes \\.\DISPLAY24
#include <windows.h>
#include <cstdio>
#include <set>
#include <tuple>

int wmain(int argc, wchar_t** argv) {
    const wchar_t* dev = argc > 1 ? argv[1] : nullptr;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &dm))
        printf("current: %lux%lu @ %lu Hz\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
    std::set<std::tuple<DWORD, DWORD, DWORD>> modes;
    for (DWORD i = 0; EnumDisplaySettingsW(dev, i, &dm); i++)
        modes.insert({dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency});
    for (auto& [w, h, f] : modes)
        if (w >= 1600) printf("  %lux%lu @ %lu Hz\n", w, h, f);
    return 0;
}
