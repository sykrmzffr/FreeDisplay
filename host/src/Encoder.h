#pragma once
#include <d3d11.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;
class Capture;

// H.264 encoder fed with the capture's NV12 frames. Implementations:
//  - AmfEncoder: AMD's AMF runtime directly, in ultra-low-latency mode.
//  - MfEncoder: any vendor's hardware encoder through Media Foundation.
class VideoEncoder {
public:
    using OutputFn = std::function<void(const uint8_t* data, size_t size, uint64_t ptsUs, bool key)>;

    virtual ~VideoEncoder() = default;
    virtual const char* Name() const = 0;

    // speed: quality-vs-speed 0 (fastest) .. 100 (best), or -1 for the encoder default.
    virtual bool Init(ID3D11Device* device, LUID adapter, UINT width, UINT height, UINT fps,
                      UINT bitrate, int speed) = 0;
    virtual void Shutdown() = 0;

    // Delivers finished frames to `fn`. Returns false if the encoder failed.
    virtual bool Pump(const OutputFn& fn) = 0;

    // True when the encoder can take another frame.
    virtual bool WantsInput() const = 0;

    // Converts the capture's latest image and submits it. Returns false on failure.
    virtual bool Submit(Capture& cap, uint64_t ptsUs) = 0;

    virtual void ForceKeyframe() = 0;
};

// Hardware H.264 encoder (async Media Foundation transform) fed with D3D11 NV12 textures.
class MfEncoder : public VideoEncoder {
public:
    ~MfEncoder() override { Shutdown(); }
    const char* Name() const override { return "Media Foundation"; }
    bool Init(ID3D11Device* device, LUID adapter, UINT width, UINT height, UINT fps,
              UINT bitrate, int speed) override;
    void Shutdown() override;
    bool Pump(const OutputFn& fn) override;
    bool WantsInput() const override { return needInput_ > 0; }
    bool Submit(Capture& cap, uint64_t ptsUs) override;
    void ForceKeyframe() override;

private:
    bool SelectTransform(LUID adapter);
    bool Configure(UINT width, UINT height, UINT fps, UINT bitrate, int speed);
    bool DrainOutput(const OutputFn& fn);

    ComPtr<IMFTransform> mft_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    ComPtr<IMFMediaType> inputType_;
    DWORD inStream_ = 0, outStream_ = 0;
    int needInput_ = 0;
    bool forceKey_ = false;
};

std::unique_ptr<VideoEncoder> CreateAmfEncoder();
