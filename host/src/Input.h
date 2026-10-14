#pragma once
#include <windows.h>
#include <map>
#include <mutex>
#include "Protocol.h"

// Injects tablet input into Windows without a driver. Coordinates are mapped onto
// one monitor's desktop rect.
//  - Fingers use legacy touch injection (InjectTouchInput). Synthetic pointer
//    devices proved unreliable on machines that have their own touch/pen screen:
//    Windows binds them to the built-in display.
//  - The pen is sent as absolute mouse input, so it draws where it touches but
//    without pressure or tilt.
class Input {
public:
    bool Init();
    void Shutdown();
    void SetTarget(const RECT& desktop);

    void Handle(const proto::InputEvent& ev);

    // Re-sends held touch contacts so Windows does not time them out.
    void KeepAlive();

private:
    struct Contact {
        UINT32 slot;
        POINT pt;
    };
    POINT Map(float x, float y) const;
    void HandleTouch(const proto::InputEvent& ev);
    void HandlePen(const proto::InputEvent& ev);
    void InjectContacts(UINT32 changedSlot, POINTER_FLAGS changedFlags);
    void SendMouse(POINT pt, DWORD buttonFlags);
    UINT32 FreeSlot() const;

    std::mutex mu_;
    bool touchReady_ = false;
    RECT target_{};
    std::map<uint32_t, Contact> contacts_; // android pointer id -> contact
    ULONGLONG lastTouchInject_ = 0;
    bool penDown_ = false;
    bool barrelDown_ = false;
};
