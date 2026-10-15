#pragma once
// Wire protocol shared with the Android client. See PROTOCOL.md.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace proto {

constexpr uint16_t kPort = 27184;
constexpr uint32_t kMagic = 0x46445350; // "FDSP"
constexpr uint16_t kVersion = 3; // 3: hello carries decoder limits; ORIENTATION message

enum MsgType : uint8_t {
    kHello = 1,
    kVideoConfig = 2,
    kVideoFrame = 3,
    kInput = 4,
    kRequestKeyframe = 6,
    kFrameStats = 7,  // tablet -> host: u64 ptsUs, u32 decodeUs, u32 inputWaitUs
    kDecoderInfo = 8, // tablet -> host: UTF-8 description of the decoder
    kPing = 9,        // host -> tablet: u64 hostUs
    kPong = 10,       // tablet -> host: the ping payload echoed back
    kOrientation = 11, // host -> tablet: u8 Orientation
};

enum Orientation : uint8_t { kOrientAuto = 0, kOrientLandscape = 1, kOrientPortrait = 2 };

enum InputAction : uint8_t {
    kActDown = 0,
    kActMove = 1,
    kActUp = 2,
    kActHoverMove = 3,
    kActHoverExit = 4,
    kActCancel = 5,
};

enum Tool : uint8_t { kFinger = 0, kPen = 1, kEraser = 2 };

struct Pointer {
    uint32_t id;
    uint8_t tool;
    float x, y;       // 0..1 over the video image
    float pressure;   // 0..1
    float tiltX, tiltY, rotation; // degrees
    uint8_t buttons;  // bit0 barrel
};

struct InputEvent {
    uint8_t action;
    uint8_t actionIndex;
    std::vector<Pointer> pointers;
};

// Big-endian writer.
class Writer {
public:
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
    void u32(uint32_t v) { u16(uint16_t(v >> 16)); u16(uint16_t(v)); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    void bytes(const void* p, size_t n) {
        auto b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
};

// Big-endian reader over a payload. Reads past the end yield zeros and set ok=false.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok = true;
    uint8_t u8() { return need(1) ? p_[i_++] : 0; }
    uint16_t u16() { uint16_t h = u8(); return uint16_t((h << 8) | u8()); }
    uint32_t u32() { uint32_t h = u16(); return (h << 16) | u16(); }
    float f32() { uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    std::string rest() {
        std::string s(reinterpret_cast<const char*>(p_ + i_), n_ - i_);
        i_ = n_;
        return s;
    }
private:
    bool need(size_t k) { if (i_ + k > n_) { ok = false; return false; } return true; }
    const uint8_t* p_;
    size_t n_;
    size_t i_ = 0;
};

inline bool ParseInput(const std::vector<uint8_t>& payload, InputEvent& ev) {
    Reader r(payload.data(), payload.size());
    ev.action = r.u8();
    ev.actionIndex = r.u8();
    uint8_t count = r.u8();
    ev.pointers.resize(count);
    for (auto& p : ev.pointers) {
        p.id = r.u32();
        p.tool = r.u8();
        p.x = r.f32();
        p.y = r.f32();
        p.pressure = r.f32();
        p.tiltX = r.f32();
        p.tiltY = r.f32();
        p.rotation = r.f32();
        p.buttons = r.u8();
    }
    return r.ok;
}

} // namespace proto
