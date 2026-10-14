// H.264 encoding through AMD's AMF runtime (amfrt64.dll, shipped with the AMD
// driver), using the ultra-low-latency usage that Media Foundation does not expose.
#include "Encoder.h"
#include "Capture.h"
#include "Util.h"

#include <amf/core/Factory.h>
#include <amf/components/VideoEncoderVCE.h>

namespace {

constexpr int kRing = 4;         // NV12 textures cycled through the encoder
constexpr int kMaxInFlight = 2;  // frames submitted but not yet returned

class AmfEncoder : public VideoEncoder {
public:
    ~AmfEncoder() override { Shutdown(); }
    const char* Name() const override { return "AMD AMF (ultra-low-latency)"; }

    bool Init(ID3D11Device* device, LUID, UINT width, UINT height, UINT fps, UINT bitrate,
              int speed) override {
        Shutdown();
        dll_ = LoadLibraryW(AMF_DLL_NAME);
        if (!dll_) {
            Log("AMF runtime (%s) not available", AMF_DLL_NAMEA);
            return false;
        }
        auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll_, AMF_INIT_FUNCTION_NAME));
        if (!init || !AmfOk(init(AMF_FULL_VERSION, &factory_), "AMFInit")) return false;
        if (!AmfOk(factory_->CreateContext(&ctx_), "CreateContext")) return false;
        if (!AmfOk(ctx_->InitDX11(device), "InitDX11")) return false;
        if (!AmfOk(factory_->CreateComponent(ctx_, AMFVideoEncoderVCE_AVC, &enc_), "CreateComponent"))
            return false;

        // Usage must be set first: it resets every other property to its preset.
        if (!AmfOk(enc_->SetProperty(AMF_VIDEO_ENCODER_USAGE,
                                     amf_int64(AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY)),
                   "usage"))
            return false;
        Set(AMF_VIDEO_ENCODER_PROFILE, amf_int64(AMF_VIDEO_ENCODER_PROFILE_MAIN));
        Set(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize(amf_int32(width), amf_int32(height)));
        Set(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(fps, 1));
        Set(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, amf_int64(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
        Set(AMF_VIDEO_ENCODER_TARGET_BITRATE, amf_int64(bitrate));
        Set(AMF_VIDEO_ENCODER_PEAK_BITRATE, amf_int64(bitrate) * 2);
        Set(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
        Set(AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf_int64(0));
        Set(AMF_VIDEO_ENCODER_IDR_PERIOD, amf_int64(fps) * 2);
        Set(AMF_VIDEO_ENCODER_QUALITY_PRESET,
            amf_int64(speed >= 0 && speed > 50 ? AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED
                                               : AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED));
        Set(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
        Set(AMF_VIDEO_ENCODER_INPUT_COLOR_PROFILE, amf_int64(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
        Set(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, amf_int64(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
        Set(AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC, amf_int64(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709));
        Set(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, amf_int64(AMF_COLOR_PRIMARIES_BT709));
        Set(AMF_VIDEO_ENCODER_OUTPUT_FULL_RANGE_COLOR, false);
        // Let QueryOutput wait up to 1 ms for a frame instead of returning at once.
        canWait_ = enc_->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, amf_int64(1)) == AMF_OK;

        if (!AmfOk(enc_->Init(amf::AMF_SURFACE_NV12, amf_int32(width), amf_int32(height)), "encoder Init"))
            return false;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (auto& t : ring_)
            if (!HR_OK(device->CreateTexture2D(&td, nullptr, &t))) return false;
        next_ = inFlight_ = 0;
        forceKey_ = true;
        Log("Encoder ready (AMF ultra-low-latency): %ux%u @ %u fps, %u kbps", width, height, fps,
            bitrate / 1000);
        return true;
    }

    void Shutdown() override {
        if (enc_) {
            enc_->Terminate();
            enc_ = nullptr;
        }
        if (ctx_) {
            ctx_->Terminate();
            ctx_ = nullptr;
        }
        factory_ = nullptr; // owned by the runtime
        for (auto& t : ring_) t.Reset();
        if (dll_) {
            FreeLibrary(dll_);
            dll_ = nullptr;
        }
        inFlight_ = 0;
    }

    bool Pump(const OutputFn& fn) override {
        while (inFlight_ > 0) {
            amf::AMFDataPtr data;
            AMF_RESULT r = enc_->QueryOutput(&data);
            if (r == AMF_REPEAT || (r == AMF_OK && !data)) {
                if (!canWait_) Sleep(0);
                return true; // nothing ready yet
            }
            if (r != AMF_OK) return AmfOk(r, "QueryOutput");
            amf::AMFBufferPtr buf(data);
            if (!buf) return true;
            amf_int64 type = -1;
            data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);
            uint64_t pts = uint64_t(data->GetPts() / 10);
            uint64_t now = NowUs();
            for (auto& [p, t] : submitted_) {
                if (p == pts) {
                    hwUs_ += now - t;
                    hwFrames_++;
                }
            }
            if (hwFrames_ == 300) {
                Log("AMF: %.1f ms from submit to output (avg of 300)", hwUs_ / 300.0 / 1000.0);
                hwUs_ = hwFrames_ = 0;
            }
            fn(static_cast<const uint8_t*>(buf->GetNative()), buf->GetSize(), pts,
               type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR);
            inFlight_--;
        }
        return true;
    }

    bool WantsInput() const override { return inFlight_ < kMaxInFlight; }

    bool Submit(Capture& cap, uint64_t ptsUs) override {
        ID3D11Texture2D* tex = ring_[next_].Get();
        if (!cap.ConvertTo(tex)) return false;
        amf::AMFSurfacePtr surface;
        if (!AmfOk(ctx_->CreateSurfaceFromDX11Native(tex, &surface, nullptr), "wrap surface")) return false;
        surface->SetPts(amf_pts(ptsUs) * 10);
        if (forceKey_) {
            surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, amf_int64(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
            forceKey_ = false;
        }
        uint64_t convertUs = NowUs();
        AMF_RESULT r = enc_->SubmitInput(surface);
        if (r == AMF_INPUT_FULL) return true; // encoder busy: drop this frame
        if (!AmfOk(r, "SubmitInput")) return false;
        submitted_[next_] = {ptsUs, convertUs};
        inFlight_++;
        next_ = (next_ + 1) % kRing;
        return true;
    }

    void ForceKeyframe() override { forceKey_ = true; }

private:
    template <typename T>
    void Set(const wchar_t* name, const T& value) {
        AMF_RESULT r = enc_->SetProperty(name, value);
        if (r != AMF_OK) Log("AMF ignores %s (%d)", Narrow(name).c_str(), int(r));
    }

    static bool AmfOk(AMF_RESULT r, const char* what) {
        if (r == AMF_OK) return true;
        Log("AMF %s failed (%d)", what, int(r));
        return false;
    }

    HMODULE dll_ = nullptr;
    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContextPtr ctx_;
    amf::AMFComponentPtr enc_;
    ComPtr<ID3D11Texture2D> ring_[kRing];
    int next_ = 0;
    int inFlight_ = 0;
    std::pair<uint64_t, uint64_t> submitted_[kRing]{}; // (pts, time submitted) per ring slot
    uint64_t hwUs_ = 0, hwFrames_ = 0;
    bool forceKey_ = false;
    bool canWait_ = false;
};

} // namespace

std::unique_ptr<VideoEncoder> CreateAmfEncoder() { return std::make_unique<AmfEncoder>(); }
