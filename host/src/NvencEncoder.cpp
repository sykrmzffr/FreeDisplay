#include "NvencEncoder.h"
#include "Capture.h"
#include "Util.h"

namespace {
using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using MaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);
} // namespace

bool NvencEncoder::Ok(NVENCSTATUS s, const char* what) const {
    if (s == NV_ENC_SUCCESS) return true;
    const char* detail = (encoder_ && api_.nvEncGetLastErrorString) ? api_.nvEncGetLastErrorString(encoder_) : "";
    Log("NVENC %s failed (%d) %s", what, int(s), detail ? detail : "");
    return false;
}

bool NvencEncoder::Init(ID3D11Device* device, LUID, UINT width, UINT height, UINT fps, UINT bitrate,
                        int speed) {
    Shutdown();
    dll_ = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!dll_) {
        Log("NVENC runtime not available");
        return false;
    }
    auto maxVersion = reinterpret_cast<MaxVersionFn>(GetProcAddress(dll_, "NvEncodeAPIGetMaxSupportedVersion"));
    auto create = reinterpret_cast<CreateInstanceFn>(GetProcAddress(dll_, "NvEncodeAPICreateInstance"));
    uint32_t driverVersion = 0;
    if (!maxVersion || !create || maxVersion(&driverVersion) != NV_ENC_SUCCESS) return false;
    uint32_t needed = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (driverVersion < needed) {
        Log("NVIDIA driver supports NVENC API %u.%u; %u.%u needed (update the driver)", driverVersion >> 4,
            driverVersion & 0xF, unsigned(NVENCAPI_MAJOR_VERSION), unsigned(NVENCAPI_MINOR_VERSION));
        return false;
    }
    api_ = {};
    api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (!Ok(create(&api_), "create instance")) return false;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device = device;
    open.apiVersion = NVENCAPI_VERSION;
    if (!Ok(api_.nvEncOpenEncodeSessionEx(&open, &encoder_), "open session")) {
        encoder_ = nullptr;
        return false;
    }

    GUID preset = speed > 50 ? NV_ENC_PRESET_P4_GUID : NV_ENC_PRESET_P1_GUID;
    NV_ENC_PRESET_CONFIG presetCfg{};
    presetCfg.version = NV_ENC_PRESET_CONFIG_VER;
    presetCfg.presetCfg.version = NV_ENC_CONFIG_VER;
    if (!Ok(api_.nvEncGetEncodePresetConfigEx(encoder_, NV_ENC_CODEC_H264_GUID, preset,
                                              NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetCfg),
            "preset config"))
        return false;

    NV_ENC_CONFIG cfg = presetCfg.presetCfg;
    cfg.version = NV_ENC_CONFIG_VER;
    cfg.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
    cfg.gopLength = NVENC_INFINITE_GOPLENGTH; // keyframes only on request
    cfg.frameIntervalP = 1;                  // no B-frames
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = bitrate;
    cfg.rcParams.maxBitRate = bitrate;
    // A one-frame VBV keeps every frame near the average size: no burst to wait for.
    cfg.rcParams.vbvBufferSize = bitrate / fps;
    cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize;
    auto& h264 = cfg.encodeCodecConfig.h264Config;
    h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    h264.repeatSPSPPS = 1;
    h264.h264VUIParameters.videoSignalTypePresentFlag = 1;
    h264.h264VUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    h264.h264VUIParameters.videoFullRangeFlag = 0;
    h264.h264VUIParameters.colourDescriptionPresentFlag = 1;
    h264.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    h264.h264VUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    h264.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init.presetGUID = preset;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth = init.darWidth = init.maxEncodeWidth = width;
    init.encodeHeight = init.darHeight = init.maxEncodeHeight = height;
    init.frameRateNum = fps;
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.encodeConfig = &cfg;
    if (!Ok(api_.nvEncInitializeEncoder(encoder_, &init), "initialize")) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (auto& slot : ring_) {
        if (!HR_OK(device->CreateTexture2D(&td, nullptr, &slot.tex))) return false;
        NV_ENC_REGISTER_RESOURCE reg{};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.width = width;
        reg.height = height;
        reg.resourceToRegister = slot.tex.Get();
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (!Ok(api_.nvEncRegisterResource(encoder_, &reg), "register texture")) return false;
        slot.registered = reg.registeredResource;
        NV_ENC_CREATE_BITSTREAM_BUFFER bs{};
        bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (!Ok(api_.nvEncCreateBitstreamBuffer(encoder_, &bs), "create bitstream")) return false;
        slot.bitstream = bs.bitstreamBuffer;
    }
    width_ = width;
    height_ = height;
    next_ = 0;
    forceKey_ = true;
    Log("Encoder ready (NVENC ultra-low-latency): %ux%u @ %u fps, %u kbps", width, height, fps, bitrate / 1000);
    return true;
}

bool NvencEncoder::EncodeTexture(int slot, uint64_t ptsUs) {
    Slot& s = ring_[slot];
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = s.registered;
    if (!Ok(api_.nvEncMapInputResource(encoder_, &map), "map input")) return false;

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = width_;
    pic.inputHeight = height_;
    pic.inputPitch = width_;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.outputBitstream = s.bitstream;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = ptsUs;
    if (forceKey_) {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        forceKey_ = false;
    }
    NVENCSTATUS st = api_.nvEncEncodePicture(encoder_, &pic);
    bool ok = Ok(st, "encode");
    if (ok) {
        // Synchronous mode: locking waits until this frame is encoded.
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = s.bitstream;
        ok = Ok(api_.nvEncLockBitstream(encoder_, &lock), "lock bitstream");
        if (ok) {
            auto* p = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
            ready_.push_back({std::vector<uint8_t>(p, p + lock.bitstreamSizeInBytes), lock.outputTimeStamp,
                              lock.pictureType == NV_ENC_PIC_TYPE_IDR});
            api_.nvEncUnlockBitstream(encoder_, s.bitstream);
        }
    }
    api_.nvEncUnmapInputResource(encoder_, map.mappedResource);
    next_ = (slot + 1) % kRing;
    return ok;
}

bool NvencEncoder::Submit(Capture& cap, uint64_t ptsUs) {
    int slot = next_;
    if (!cap.ConvertTo(ring_[slot].tex.Get())) return false;
    return EncodeTexture(slot, ptsUs);
}

bool NvencEncoder::Pump(const OutputFn& fn) {
    for (auto& p : ready_) fn(p.data.data(), p.data.size(), p.ptsUs, p.key);
    ready_.clear();
    return true;
}

void NvencEncoder::Shutdown() {
    if (encoder_) {
        for (auto& slot : ring_) {
            if (slot.registered) api_.nvEncUnregisterResource(encoder_, slot.registered);
            if (slot.bitstream) api_.nvEncDestroyBitstreamBuffer(encoder_, slot.bitstream);
            slot.registered = nullptr;
            slot.bitstream = nullptr;
        }
        api_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }
    for (auto& slot : ring_) slot.tex.Reset();
    ready_.clear();
    if (dll_) {
        FreeLibrary(dll_);
        dll_ = nullptr;
    }
}
