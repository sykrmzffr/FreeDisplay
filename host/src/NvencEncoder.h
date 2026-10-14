#pragma once
#include "Encoder.h"

#include <nvenc/nvEncodeAPI.h>
#include <vector>

// H.264 encoding through NVIDIA's NVENC API (nvEncodeAPI64.dll, shipped with the
// driver) using the ultra-low-latency tuning. Media Foundation's NVIDIA encoder
// can refuse to start on hybrid-GPU laptops; this path talks to NVENC directly.
class NvencEncoder : public VideoEncoder {
public:
    ~NvencEncoder() override { Shutdown(); }
    const char* Name() const override { return "NVIDIA NVENC (ultra-low-latency)"; }
    bool Init(ID3D11Device* device, LUID adapter, UINT width, UINT height, UINT fps,
              UINT bitrate, int speed) override;
    void Shutdown() override;
    bool Pump(const OutputFn& fn) override;
    bool WantsInput() const override { return encoder_ != nullptr; }
    bool Submit(Capture& cap, uint64_t ptsUs) override;
    void ForceKeyframe() override { forceKey_ = true; }

    // Encodes one NV12 texture (from Textures()) synchronously. Exposed for benchmarks.
    bool EncodeTexture(int slot, uint64_t ptsUs);
    ID3D11Texture2D* Texture(int slot) const { return ring_[slot].tex.Get(); }
    int NextSlot() const { return next_; }

private:
    static constexpr int kRing = 3;
    struct Slot {
        ComPtr<ID3D11Texture2D> tex;
        NV_ENC_REGISTERED_PTR registered = nullptr;
        NV_ENC_OUTPUT_PTR bitstream = nullptr;
    };
    struct Packet {
        std::vector<uint8_t> data;
        uint64_t ptsUs;
        bool key;
    };
    bool Ok(NVENCSTATUS s, const char* what) const;

    HMODULE dll_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* encoder_ = nullptr;
    Slot ring_[kRing];
    int next_ = 0;
    UINT width_ = 0, height_ = 0;
    bool forceKey_ = true;
    std::vector<Packet> ready_;
};
