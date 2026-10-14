#include "Encoder.h"
#include "Capture.h"
#include "Util.h"
#include <codecapi.h>
#include <mferror.h>
#include <strmif.h>

namespace {

void SetCodecValue(ICodecAPI* api, const GUID& key, UINT32 value, const char* name) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    HRESULT hr = api->SetValue(&key, &v);
    if (FAILED(hr)) Log("Encoder ignores %s (0x%08lX)", name, static_cast<unsigned long>(hr));
}

void SetCodecBool(ICodecAPI* api, const GUID& key, bool value, const char* name) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    HRESULT hr = api->SetValue(&key, &v);
    if (FAILED(hr)) Log("Encoder ignores %s (0x%08lX)", name, static_cast<unsigned long>(hr));
}

} // namespace

bool MfEncoder::SelectTransform(LUID adapter) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_H264};
    UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;

    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    // Prefer the encoder that lives on the same GPU as the captured output.
    ComPtr<IMFAttributes> filter;
    if (SUCCEEDED(MFCreateAttributes(&filter, 1)) &&
        SUCCEEDED(filter->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<UINT8*>(&adapter),
                                  sizeof adapter))) {
        MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, filter.Get(), &acts, &count);
    }
    if (count == 0) {
        CoTaskMemFree(acts);
        acts = nullptr;
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &acts, &count);
    }
    if (count == 0) {
        Log("No hardware H.264 encoder found");
        CoTaskMemFree(acts);
        return false;
    }

    Log("Found %u hardware H.264 encoder(s)", count);
    bool ok = false;
    for (UINT32 i = 0; i < count && !ok; i++) {
        wchar_t* name = nullptr;
        UINT32 len = 0;
        acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len);
        HRESULT hr = acts[i]->ActivateObject(IID_PPV_ARGS(&mft_));
        if (SUCCEEDED(hr)) {
            Log("Using encoder: %s", name ? Narrow(name).c_str() : "(unnamed)");
            ok = true;
        } else {
            Log("Encoder %s failed to activate (0x%08lX)", name ? Narrow(name).c_str() : "(unnamed)",
                static_cast<unsigned long>(hr));
        }
        CoTaskMemFree(name);
    }
    for (UINT32 i = 0; i < count; i++) acts[i]->Release();
    CoTaskMemFree(acts);
    return ok;
}

bool MfEncoder::Init(ID3D11Device* device, LUID adapter, UINT width, UINT height, UINT fps,
                   UINT bitrate, int speed) {
    Shutdown();
    if (!SelectTransform(adapter)) return false;

    // Hardware encoders are asynchronous and must be unlocked before use.
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
        UINT32 async = 0;
        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async);
        if (!async) {
            Log("Encoder is synchronous; only async hardware encoders are supported");
            return false;
        }
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    if (!HR_OK(mft_.As(&events_))) return false;

    UINT token = 0;
    if (!HR_OK(MFCreateDXGIDeviceManager(&token, &manager_))) return false;
    if (!HR_OK(manager_->ResetDevice(device, token))) return false;
    if (!HR_OK(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                    reinterpret_cast<ULONG_PTR>(manager_.Get()))))
        return false;

    HRESULT hr = mft_->GetStreamIDs(1, &inStream_, 1, &outStream_);
    if (hr == E_NOTIMPL) inStream_ = outStream_ = 0;

    if (!Configure(width, height, fps, bitrate, speed)) return false;

    if (!HR_OK(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_)))) return false;
    allocator_->SetDirectXManager(manager_.Get());
    ComPtr<IMFAttributes> allocAttrs;
    MFCreateAttributes(&allocAttrs, 2);
    allocAttrs->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET);
    allocAttrs->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
    if (!HR_OK(allocator_->InitializeSampleAllocatorEx(4, 10, allocAttrs.Get(), inputType_.Get())))
        return false;

    if (!HR_OK(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return false;
    if (!HR_OK(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return false;
    needInput_ = 0;
    forceKey_ = false;
    Log("Encoder ready: %ux%u @ %u fps, %u kbps", width, height, fps, bitrate / 1000);
    return true;
}

bool MfEncoder::Configure(UINT width, UINT height, UINT fps, UINT bitrate, int speed) {
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(mft_.As(&api))) {
        SetCodecValue(api.Get(), CODECAPI_AVEncCommonRateControlMode,
                      eAVEncCommonRateControlMode_CBR, "CBR rate control");
        SetCodecValue(api.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate, "bitrate");
        SetCodecBool(api.Get(), CODECAPI_AVLowLatencyMode, true, "low-latency mode");
        SetCodecValue(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0, "no B-frames");
        SetCodecValue(api.Get(), CODECAPI_AVEncMPVGOPSize, fps * 2, "GOP size");
        if (speed >= 0)
            SetCodecValue(api.Get(), CODECAPI_AVEncCommonQualityVsSpeed, UINT32(speed), "quality/speed");
    }

    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    outType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    outType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    outType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    outType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (!HR_OK(mft_->SetOutputType(outStream_, outType.Get(), 0))) return false;

    for (DWORD i = 0;; i++) {
        ComPtr<IMFMediaType> t;
        if (FAILED(mft_->GetInputAvailableType(inStream_, i, &t))) break;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub != MFVideoFormat_NV12) continue;
        MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, fps, 1);
        if (HR_OK(mft_->SetInputType(inStream_, t.Get(), 0))) {
            inputType_ = t;
            return true;
        }
    }
    Log("Encoder rejected NV12 input at %ux%u", width, height);
    return false;
}

bool MfEncoder::Pump(const OutputFn& fn) {
    for (;;) {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) return true;
        if (!HR_OK(hr)) return false;
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == METransformNeedInput) {
            needInput_++;
        } else if (type == METransformHaveOutput) {
            if (!DrainOutput(fn)) return false;
        } else if (type == MEError) {
            HRESULT status = S_OK;
            ev->GetStatus(&status);
            Log("Encoder error event 0x%08lX", static_cast<unsigned long>(status));
            return false;
        }
    }
}

bool MfEncoder::DrainOutput(const OutputFn& fn) {
    MFT_OUTPUT_STREAM_INFO info{};
    mft_->GetOutputStreamInfo(outStream_, &info);
    bool providesSamples =
        (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    MFT_OUTPUT_DATA_BUFFER out{};
    out.dwStreamID = outStream_;
    ComPtr<IMFSample> own;
    if (!providesSamples) {
        ComPtr<IMFMediaBuffer> buf;
        MFCreateSample(&own);
        MFCreateMemoryBuffer(info.cbSize ? info.cbSize : 4 * 1024 * 1024, &buf);
        own->AddBuffer(buf.Get());
        out.pSample = own.Get();
    }
    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
    if (out.pEvents) out.pEvents->Release();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        ComPtr<IMFMediaType> t;
        if (SUCCEEDED(mft_->GetOutputAvailableType(outStream_, 0, &t)))
            mft_->SetOutputType(outStream_, t.Get(), 0);
        if (providesSamples && out.pSample) out.pSample->Release();
        return true;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
    if (!HR_OK(hr)) return false;

    IMFSample* sample = out.pSample;
    ComPtr<IMFMediaBuffer> contiguous;
    if (sample && SUCCEEDED(sample->ConvertToContiguousBuffer(&contiguous))) {
        BYTE* data = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(contiguous->Lock(&data, nullptr, &len))) {
            LONGLONG time = 0;
            sample->GetSampleTime(&time);
            UINT32 clean = 0;
            sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
            fn(data, len, uint64_t(time / 10), clean != 0);
            contiguous->Unlock();
        }
    }
    if (providesSamples && sample) sample->Release();
    return true;
}

bool MfEncoder::Submit(Capture& cap, uint64_t ptsUs) {
    ComPtr<IMFSample> sample;
    HRESULT hr = allocator_->AllocateSample(&sample);
    if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) return true; // encoder still holds every texture; skip
    if (!HR_OK(hr)) return false;

    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMFDXGIBuffer> dxgi;
    ComPtr<ID3D11Texture2D> tex;
    if (!HR_OK(sample->GetBufferByIndex(0, &buf)) || !HR_OK(buf.As(&dxgi)) ||
        !HR_OK(dxgi->GetResource(IID_PPV_ARGS(&tex))))
        return false;
    if (!cap.ConvertTo(tex.Get())) return false;

    sample->SetSampleTime(LONGLONG(ptsUs) * 10);
    sample->SetSampleDuration(10 * 1000 * 1000 / 60);
    if (forceKey_) {
        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(mft_.As(&api))) SetCodecValue(api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1, "force keyframe");
        forceKey_ = false;
    }
    if (!HR_OK(mft_->ProcessInput(inStream_, sample.Get(), 0))) return false;
    needInput_--;
    return true;
}

void MfEncoder::ForceKeyframe() { forceKey_ = true; }

void MfEncoder::Shutdown() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        ComPtr<IMFShutdown> sd;
        if (SUCCEEDED(mft_.As(&sd))) sd->Shutdown();
    }
    if (allocator_) allocator_->UninitializeSampleAllocator();
    allocator_.Reset();
    inputType_.Reset();
    events_.Reset();
    manager_.Reset();
    mft_.Reset();
    needInput_ = 0;
}
