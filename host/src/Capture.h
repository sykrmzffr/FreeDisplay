#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct OutputInfo {
    int index;                  // stable index across adapters, for --display
    std::wstring gdiName;       // \\.\DISPLAYn
    std::wstring monitorName;   // EDID friendly name, e.g. "VDD by MTT"
    std::wstring adapterName;
    RECT desktop;               // desktop coordinates (physical pixels)
    bool primary;
};

std::vector<OutputInfo> ListOutputs();

// Picks the output to stream: by index if >= 0, else the first VDD monitor, else primary.
bool ChooseOutput(int requested, OutputInfo& chosen);

// Captures one output with Desktop Duplication and converts each frame to NV12 at
// the encode size on the GPU. Frames land in caller-supplied NV12 textures.
class Capture {
public:
    bool Init(const OutputInfo& out, UINT encodeW, UINT encodeH);
    void Shutdown();

    // After the duplication was lost (mode change, Win+P, secure desktop): duplicates
    // the output again on the same D3D device, which the encoder keeps using. Returns
    // false when that is not possible (output gone, other GPU, other size); the
    // session must then restart.
    bool Reacquire(const OutputInfo& out);

    ID3D11Device* Device() const { return device_.Get(); }
    LUID AdapterLuid() const { return luid_; }
    UINT AdapterVendor() const { return vendor_; } // PCI vendor id: 0x1002 AMD, 0x10DE NVIDIA, 0x8086 Intel
    UINT SourceWidth() const { return srcW_; }
    UINT SourceHeight() const { return srcH_; }

    // Waits up to timeoutMs for a new desktop image. Returns 1 for a new frame,
    // 0 on timeout, -1 if duplication was lost (call Init again).
    int Acquire(UINT timeoutMs);

    // Converts the most recent desktop image into `nv12` (encode size).
    bool ConvertTo(ID3D11Texture2D* nv12);

    bool HasFrame() const { return haveFrame_; }

private:
    bool CreateProcessor();
    // Rebuilds copy_ from desktop_ with the mouse pointer drawn on top.
    void Compose();
    void BlendCursor(uint8_t* dst, UINT dstPitch, int originX, int originY, UINT w, UINT h);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> desktop_;  // BGRA copy of the latest desktop image
    ComPtr<ID3D11Texture2D> copy_;     // desktop_ plus the cursor; video processor input
    ComPtr<ID3D11Texture2D> staging_;  // CPU-readable patch under the cursor

    // Desktop Duplication leaves the cursor out of frames, so we draw it ourselves.
    std::vector<uint8_t> cursorShape_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO cursorInfo_{};
    POINT cursorPos_{};
    bool cursorVisible_ = false;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11VideoProcessorInputView> inView_;
    std::vector<std::pair<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorOutputView>>> outViews_;
    LUID luid_{};
    UINT vendor_ = 0;
    UINT srcW_ = 0, srcH_ = 0, dstW_ = 0, dstH_ = 0;
    bool haveFrame_ = false;
};
