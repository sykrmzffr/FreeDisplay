#include "Capture.h"
#include "Util.h"
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <map>

namespace {

// GDI device name (\\.\DISPLAYn) -> monitor friendly name from the EDID.
std::map<std::wstring, std::wstring> MonitorNames() {
    std::map<std::wstring, std::wstring> names;
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS)
        return names;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(),
                           nullptr) != ERROR_SUCCESS)
        return names;
    for (UINT32 i = 0; i < nPaths; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof tgt;
        tgt.header.adapterId = paths[i].targetInfo.adapterId;
        tgt.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS &&
            DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS)
            names[src.viewGdiDeviceName] = tgt.monitorFriendlyDeviceName;
    }
    return names;
}

// Finds the DXGI adapter and output for a GDI device name.
bool FindOutput(const std::wstring& gdiName, ComPtr<IDXGIAdapter1>& adapter,
                ComPtr<IDXGIOutput1>& output) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> a;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &a) != DXGI_ERROR_NOT_FOUND; ai++) {
        ComPtr<IDXGIOutput> o;
        for (UINT oi = 0; a->EnumOutputs(oi, &o) != DXGI_ERROR_NOT_FOUND; oi++) {
            DXGI_OUTPUT_DESC d;
            o->GetDesc(&d);
            if (gdiName == d.DeviceName && SUCCEEDED(o.As(&output))) {
                adapter = a;
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::vector<OutputInfo> ListOutputs() {
    std::vector<OutputInfo> list;
    auto names = MonitorNames();
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return list;
    ComPtr<IDXGIAdapter1> a;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &a) != DXGI_ERROR_NOT_FOUND; ai++) {
        DXGI_ADAPTER_DESC1 ad;
        a->GetDesc1(&ad);
        ComPtr<IDXGIOutput> o;
        for (UINT oi = 0; a->EnumOutputs(oi, &o) != DXGI_ERROR_NOT_FOUND; oi++) {
            DXGI_OUTPUT_DESC d;
            o->GetDesc(&d);
            if (!d.AttachedToDesktop) continue;
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(d.Monitor, &mi);
            OutputInfo info;
            info.index = int(list.size());
            info.gdiName = d.DeviceName;
            info.monitorName = names.count(d.DeviceName) ? names[d.DeviceName] : L"";
            info.adapterName = ad.Description;
            info.desktop = d.DesktopCoordinates;
            info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            list.push_back(info);
        }
    }
    return list;
}

bool ChooseOutput(int requested, OutputInfo& chosen) {
    auto list = ListOutputs();
    if (list.empty()) return false;
    if (requested >= 0) {
        if (requested >= int(list.size())) return false;
        chosen = list[requested];
        return true;
    }
    for (auto& o : list) {
        if (o.monitorName.find(L"VDD") != std::wstring::npos) {
            chosen = o;
            return true;
        }
    }
    for (auto& o : list) {
        if (o.primary) {
            chosen = o;
            return true;
        }
    }
    chosen = list[0];
    return true;
}

bool Capture::Init(const OutputInfo& out, UINT encodeW, UINT encodeH) {
    Shutdown();
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    if (!FindOutput(out.gdiName, adapter, output)) {
        Log("Output %s not found", Narrow(out.gdiName).c_str());
        return false;
    }
    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);
    luid_ = ad.AdapterLuid;
    vendor_ = ad.VendorId;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (!HR_OK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                 ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, &ctx_)))
        return false;
    // Media Foundation uses the same device from its own threads.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);
    if (!HR_OK(device_.As(&vdev_)) || !HR_OK(ctx_.As(&vctx_))) return false;

    Log("Capture: D3D11 device ready, starting duplication");
    ComPtr<IDXGIOutput5> output5;
    DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
    HRESULT hr = E_NOINTERFACE;
    if (SUCCEEDED(output.As(&output5)))
        hr = output5->DuplicateOutput1(device_.Get(), 0, ARRAYSIZE(formats), formats, &dup_);
    if (FAILED(hr)) hr = output->DuplicateOutput(device_.Get(), &dup_);
    if (!HR_OK(hr)) return false;

    DXGI_OUTDUPL_DESC dd;
    dup_->GetDesc(&dd);
    srcW_ = dd.ModeDesc.Width;
    srcH_ = dd.ModeDesc.Height;
    dstW_ = encodeW;
    dstH_ = encodeH;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = srcW_;
    td.Height = srcH_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (!HR_OK(device_->CreateTexture2D(&td, nullptr, &copy_))) return false;
    if (!HR_OK(device_->CreateTexture2D(&td, nullptr, &desktop_))) return false;
    // Start black so the first keyframe is valid even before the desktop changes.
    for (auto* tex : {copy_.Get(), desktop_.Get()}) {
        ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(device_->CreateRenderTargetView(tex, nullptr, &rtv))) {
            float black[4] = {0, 0, 0, 1};
            ctx_->ClearRenderTargetView(rtv.Get(), black);
        }
    }
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Width = sd.Height = 256; // largest cursor we draw
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    if (!HR_OK(device_->CreateTexture2D(&sd, nullptr, &staging_))) return false;
    cursorShape_.clear();
    cursorVisible_ = false;
    haveFrame_ = false;

    if (!CreateProcessor()) return false;
    Log("Capturing %s (%s) %ux%u on %s -> encode %ux%u", Narrow(out.gdiName).c_str(),
        Narrow(out.monitorName).c_str(), srcW_, srcH_, Narrow(ad.Description).c_str(), dstW_,
        dstH_);
    return true;
}

bool Capture::CreateProcessor() {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = srcW_;
    cd.InputHeight = srcH_;
    cd.OutputWidth = dstW_;
    cd.OutputHeight = dstH_;
    cd.InputFrameRate = {60, 1};
    cd.OutputFrameRate = {60, 1};
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (!HR_OK(vdev_->CreateVideoProcessorEnumerator(&cd, &vpEnum_))) return false;
    if (!HR_OK(vdev_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_))) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (!HR_OK(vdev_->CreateVideoProcessorInputView(copy_.Get(), vpEnum_.Get(), &ivd, &inView_)))
        return false;

    // RGB full range in, BT.709 limited range out (what the encoder signals).
    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(vctx_.As(&vctx1))) {
        vctx1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0,
                                                  DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vctx1->VideoProcessorSetOutputColorSpace1(vp_.Get(),
                                                  DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, outCs{};
        in.RGB_Range = 0;          // full
        outCs.YCbCr_Matrix = 1;    // BT.709
        outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
        vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &outCs);
    }
    RECT src{0, 0, LONG(srcW_), LONG(srcH_)};
    RECT dst{0, 0, LONG(dstW_), LONG(dstH_)};
    vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
    vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
    vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &dst);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
    return true;
}

int Capture::Acquire(UINT timeoutMs) {
    if (!dup_) return -1;
    DXGI_OUTDUPL_FRAME_INFO fi;
    ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
    if (FAILED(hr)) {
        Log("AcquireNextFrame failed 0x%08lX (display mode change or desktop switch)",
            static_cast<unsigned long>(hr));
        dup_.Reset();
        return -1;
    }
    bool changed = false;
    // LastPresentTime == 0 means only the mouse moved; the image is unchanged.
    if (fi.LastPresentTime.QuadPart != 0) {
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(res.As(&tex))) {
            ctx_->CopyResource(desktop_.Get(), tex.Get());
            haveFrame_ = true;
            changed = true;
        }
    }
    if (fi.LastMouseUpdateTime.QuadPart != 0) {
        bool visible = fi.PointerPosition.Visible != FALSE;
        if (visible != cursorVisible_ || fi.PointerPosition.Position.x != cursorPos_.x ||
            fi.PointerPosition.Position.y != cursorPos_.y)
            changed = true;
        cursorVisible_ = visible;
        cursorPos_ = fi.PointerPosition.Position;
    }
    if (fi.PointerShapeBufferSize > 0) {
        cursorShape_.resize(fi.PointerShapeBufferSize);
        UINT needed = 0;
        if (SUCCEEDED(dup_->GetFramePointerShape(UINT(cursorShape_.size()), cursorShape_.data(),
                                                 &needed, &cursorInfo_)))
            changed = true;
        else
            cursorShape_.clear();
    }
    dup_->ReleaseFrame();
    if (changed) Compose();
    return changed ? 1 : 0;
}

void Capture::Compose() {
    ctx_->CopyResource(copy_.Get(), desktop_.Get());
    if (!cursorVisible_ || cursorShape_.empty()) return;

    UINT w = cursorInfo_.Width;
    UINT h = cursorInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? cursorInfo_.Height / 2
                                                                           : cursorInfo_.Height;
    LONG x0 = std::max<LONG>(cursorPos_.x, 0), y0 = std::max<LONG>(cursorPos_.y, 0);
    LONG x1 = std::min<LONG>({cursorPos_.x + LONG(w), LONG(srcW_), x0 + 256});
    LONG y1 = std::min<LONG>({cursorPos_.y + LONG(h), LONG(srcH_), y0 + 256});
    if (x1 <= x0 || y1 <= y0) return;

    D3D11_BOX box{UINT(x0), UINT(y0), 0, UINT(x1), UINT(y1), 1};
    ctx_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, desktop_.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ_WRITE, 0, &m))) return;
    BlendCursor(static_cast<uint8_t*>(m.pData), m.RowPitch, x0 - cursorPos_.x, y0 - cursorPos_.y,
                UINT(x1 - x0), UINT(y1 - y0));
    ctx_->Unmap(staging_.Get(), 0);
    D3D11_BOX src{0, 0, 0, UINT(x1 - x0), UINT(y1 - y0), 1};
    ctx_->CopySubresourceRegion(copy_.Get(), 0, UINT(x0), UINT(y0), 0, staging_.Get(), 0, &src);
}

// Blends the cursor shape into a BGRA patch. (originX, originY) is the patch's
// top-left in cursor-shape coordinates.
void Capture::BlendCursor(uint8_t* dst, UINT dstPitch, int originX, int originY, UINT w, UINT h) {
    const uint8_t* shape = cursorShape_.data();
    UINT pitch = cursorInfo_.Pitch;
    UINT halfH = cursorInfo_.Height / 2;
    for (UINT y = 0; y < h; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(dst + y * dstPitch);
        UINT sy = UINT(originY) + y;
        for (UINT x = 0; x < w; x++) {
            UINT sx = UINT(originX) + x;
            uint32_t& px = row[x];
            switch (cursorInfo_.Type) {
            case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: {
                uint32_t c = reinterpret_cast<const uint32_t*>(shape + sy * pitch)[sx];
                uint32_t a = c >> 24;
                if (a == 0) break;
                uint32_t out = 0xFF000000;
                for (int s = 0; s < 24; s += 8) {
                    uint32_t sc = (c >> s) & 0xFF, dc = (px >> s) & 0xFF;
                    out |= ((sc * a + dc * (255 - a)) / 255) << s;
                }
                px = out;
                break;
            }
            case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
                uint32_t c = reinterpret_cast<const uint32_t*>(shape + sy * pitch)[sx];
                if ((c >> 24) == 0)
                    px = 0xFF000000 | (c & 0xFFFFFF);
                else
                    px = 0xFF000000 | ((px ^ c) & 0xFFFFFF);
                break;
            }
            case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
                uint8_t bit = uint8_t(0x80 >> (sx % 8));
                bool andBit = (shape[sy * pitch + sx / 8] & bit) != 0;
                bool xorBit = (shape[(sy + halfH) * pitch + sx / 8] & bit) != 0;
                uint32_t rgb = andBit ? (px & 0xFFFFFF) : 0;
                if (xorBit) rgb ^= 0xFFFFFF;
                px = 0xFF000000 | rgb;
                break;
            }
            }
        }
    }
}

bool Capture::ConvertTo(ID3D11Texture2D* nv12) {
    ID3D11VideoProcessorOutputView* view = nullptr;
    for (auto& [tex, v] : outViews_)
        if (tex == nv12) view = v.Get();
    if (!view) {
        if (outViews_.size() > 32) outViews_.clear();
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> v;
        if (!HR_OK(vdev_->CreateVideoProcessorOutputView(nv12, vpEnum_.Get(), &ovd, &v)))
            return false;
        view = v.Get();
        outViews_.emplace_back(nv12, v);
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView_.Get();
    return HR_OK(vctx_->VideoProcessorBlt(vp_.Get(), view, 0, 1, &stream));
}

void Capture::Shutdown() {
    outViews_.clear();
    inView_.Reset();
    vp_.Reset();
    vpEnum_.Reset();
    staging_.Reset();
    desktop_.Reset();
    copy_.Reset();
    dup_.Reset();
    vctx_.Reset();
    vdev_.Reset();
    ctx_.Reset();
    device_.Reset();
    haveFrame_ = false;
}
