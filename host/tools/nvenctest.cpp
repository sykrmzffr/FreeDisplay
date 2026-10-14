// Benchmark: encodes changing synthetic NV12 frames with NVENC on the NVIDIA GPU
// and reports per-frame encode latency, without touching the display setup.
// Usage: nvenctest [width height]
#include <windows.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <random>
#include <vector>
#include "../src/NvencEncoder.h"
#include "../src/Util.h"

int wmain(int argc, wchar_t** argv) {
    UINT w = argc > 2 ? _wtoi(argv[1]) : 2400, h = argc > 2 ? _wtoi(argv[2]) : 1602;
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter, a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (d.VendorId == 0x10DE) {
            adapter = a;
            Log("Using %s", Narrow(d.Description).c_str());
            break;
        }
    }
    if (!adapter) {
        Log("No NVIDIA GPU");
        return 1;
    }
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        Log("D3D11CreateDevice failed");
        return 1;
    }

    NvencEncoder enc;
    if (!enc.Init(dev.Get(), LUID{}, w, h, 120, 16000000, -1)) return 1;

    // A desktop-like frame: flat background with a block of detail that moves each frame.
    std::vector<uint8_t> nv12(size_t(w) * h * 3 / 2, 128);
    std::mt19937 rng(1);
    for (size_t i = 0; i < size_t(w) * h; i++) nv12[i] = uint8_t(200 + (i % w) / 64);

    std::vector<double> times;
    for (int f = 0; f < 240; f++) {
        UINT bx = (f * 37) % (w - 400), by = (f * 23) % (h - 300);
        for (UINT y = by; y < by + 300; y++)
            for (UINT x = bx; x < bx + 400; x++) nv12[size_t(y) * w + x] = uint8_t(rng());
        int slot = enc.NextSlot();
        ctx->UpdateSubresource(enc.Texture(slot), 0, nullptr, nv12.data(), w, 0);
        ctx->Flush();
        uint64_t t0 = NowUs();
        if (f % 120 == 0) enc.ForceKeyframe();
        if (!enc.EncodeTexture(slot, f * 8333)) return 1;
        uint64_t t1 = NowUs();
        size_t bytes = 0;
        enc.Pump([&](const uint8_t*, size_t n, uint64_t, bool) { bytes = n; });
        if (f >= 20) times.push_back((t1 - t0) / 1000.0);
        Sleep(8);
    }
    std::sort(times.begin(), times.end());
    double sum = 0;
    for (double t : times) sum += t;
    Log("NVENC %ux%u: encode avg %.2f ms, median %.2f ms, p95 %.2f ms, max %.2f ms (%zu frames)", w, h,
        sum / times.size(), times[times.size() / 2], times[times.size() * 95 / 100], times.back(), times.size());
    return 0;
}
