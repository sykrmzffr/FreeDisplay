#include "Input.h"
#include "Settings.h"
#include "Util.h"
#include <algorithm>
#include <vector>

namespace {
constexpr UINT32 kMaxContacts = 10;
}

bool Input::Init() {
    std::lock_guard<std::mutex> lock(mu_);
    touchReady_ = InitializeTouchInjection(kMaxContacts, TOUCH_FEEDBACK_DEFAULT) != FALSE;
    if (!touchReady_) Log("InitializeTouchInjection failed (%lu)", GetLastError());
    return touchReady_;
}

void Input::Shutdown() {
    std::lock_guard<std::mutex> lock(mu_);
    Log("Input this session: touch %u injected / %u ignored, pen %u injected / %u ignored", touchEvents_,
        touchIgnored_, penEvents_, penIgnored_);
    touchEvents_ = touchIgnored_ = penEvents_ = penIgnored_ = 0;
    if (!contacts_.empty()) InjectContacts(kMaxContacts, POINTER_FLAG_UP);
    contacts_.clear();
    if (penDown_ || barrelDown_) {
        POINT cur{};
        GetCursorPos(&cur);
        SendMouse(cur, (penDown_ ? MOUSEEVENTF_LEFTUP : 0) | (barrelDown_ ? MOUSEEVENTF_RIGHTUP : 0));
    }
    penDown_ = barrelDown_ = false;
}

void Input::SetTarget(const RECT& desktop) {
    std::lock_guard<std::mutex> lock(mu_);
    target_ = desktop;
}

POINT Input::Map(float x, float y) const {
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    LONG w = target_.right - target_.left;
    LONG h = target_.bottom - target_.top;
    return POINT{target_.left + std::min(LONG(x * w), w - 1),
                 target_.top + std::min(LONG(y * h), h - 1)};
}

void Input::Handle(const proto::InputEvent& ev) {
    if (ev.pointers.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    uint8_t tool = ev.pointers[std::min<size_t>(ev.actionIndex, ev.pointers.size() - 1)].tool;
    if (tool == proto::kFinger) {
        if (AppSettings().touch) {
            touchEvents_++;
            HandleTouch(ev);
            return;
        }
        touchIgnored_++;
        if (!contacts_.empty()) { // switched off mid-gesture: lift every finger
            InjectContacts(kMaxContacts, POINTER_FLAG_UP);
            contacts_.clear();
        }
    } else {
        if (AppSettings().pen) {
            penEvents_++;
            HandlePen(ev);
            return;
        }
        penIgnored_++;
        if (penDown_ || barrelDown_) { // switched off mid-stroke: release the button
            POINT cur{};
            GetCursorPos(&cur);
            SendMouse(cur, (penDown_ ? MOUSEEVENTF_LEFTUP : 0) | (barrelDown_ ? MOUSEEVENTF_RIGHTUP : 0));
            penDown_ = barrelDown_ = false;
        }
    }
}

UINT32 Input::FreeSlot() const {
    for (UINT32 s = 0; s < kMaxContacts; s++) {
        bool used = false;
        for (auto& [id, c] : contacts_) used |= (c.slot == s);
        if (!used) return s;
    }
    return kMaxContacts;
}

void Input::HandleTouch(const proto::InputEvent& ev) {
    if (!touchReady_) return;
    const auto& actor = ev.pointers[std::min<size_t>(ev.actionIndex, ev.pointers.size() - 1)];

    // Refresh positions of every finger in the event.
    for (auto& p : ev.pointers) {
        if (p.tool != proto::kFinger) continue;
        auto it = contacts_.find(p.id);
        if (it != contacts_.end()) it->second.pt = Map(p.x, p.y);
    }

    switch (ev.action) {
    case proto::kActDown: {
        if (contacts_.count(actor.id)) break;
        UINT32 slot = FreeSlot();
        if (slot >= kMaxContacts) return;
        contacts_[actor.id] = Contact{slot, Map(actor.x, actor.y)};
        InjectContacts(slot, POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
        break;
    }
    case proto::kActMove:
        InjectContacts(kMaxContacts, 0);
        break;
    case proto::kActUp:
    case proto::kActCancel: {
        auto it = contacts_.find(actor.id);
        if (it == contacts_.end()) break;
        POINTER_FLAGS f = POINTER_FLAG_UP;
        if (ev.action == proto::kActCancel) f |= POINTER_FLAG_CANCELED;
        InjectContacts(it->second.slot, f);
        contacts_.erase(it);
        if (ev.action == proto::kActCancel) contacts_.clear();
        break;
    }
    default:
        break;
    }
}

// Touch injection must describe every active contact in one call.
void Input::InjectContacts(UINT32 changedSlot, POINTER_FLAGS changedFlags) {
    std::vector<POINTER_TOUCH_INFO> infos;
    for (auto& [id, c] : contacts_) {
        POINTER_TOUCH_INFO t{};
        t.pointerInfo.pointerType = PT_TOUCH;
        t.pointerInfo.pointerId = c.slot;
        t.pointerInfo.ptPixelLocation = c.pt;
        t.pointerInfo.pointerFlags = (c.slot == changedSlot || (changedSlot == kMaxContacts && changedFlags != 0))
                                         ? changedFlags
                                         : POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE |
                                               POINTER_FLAG_INCONTACT;
        t.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
        t.rcContact = RECT{c.pt.x - 2, c.pt.y - 2, c.pt.x + 2, c.pt.y + 2};
        t.orientation = 90;
        t.pressure = 32000;
        infos.push_back(t);
    }
    if (infos.empty()) return;
    if (!InjectTouchInput(UINT32(infos.size()), infos.data()))
        Log("Touch injection failed (%lu)", GetLastError());
    lastTouchInject_ = GetTickCount64();
}

void Input::SendMouse(POINT pt, DWORD buttonFlags) {
    // Absolute coordinates span the whole virtual desktop, 0..65535.
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = LONG((int64_t(pt.x - vx) * 65535) / std::max(vw - 1, 1));
    in.mi.dy = LONG((int64_t(pt.y - vy) * 65535) / std::max(vh - 1, 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | buttonFlags;
    SendInput(1, &in, sizeof in);
}

void Input::HandlePen(const proto::InputEvent& ev) {
    const auto& p = ev.pointers[std::min<size_t>(ev.actionIndex, ev.pointers.size() - 1)];
    POINT pt = Map(p.x, p.y);
    bool barrel = (p.buttons & 1) != 0;
    DWORD flags = 0;
    switch (ev.action) {
    case proto::kActDown:
    case proto::kActMove:
        // Barrel button held while touching acts as a right click.
        if (!penDown_ && !barrelDown_) {
            if (barrel) { flags = MOUSEEVENTF_RIGHTDOWN; barrelDown_ = true; }
            else { flags = MOUSEEVENTF_LEFTDOWN; penDown_ = true; }
        }
        break;
    case proto::kActUp:
    case proto::kActCancel:
        if (penDown_) flags |= MOUSEEVENTF_LEFTUP;
        if (barrelDown_) flags |= MOUSEEVENTF_RIGHTUP;
        penDown_ = barrelDown_ = false;
        break;
    case proto::kActHoverMove:
        break;
    case proto::kActHoverExit:
        return;
    default:
        return;
    }
    SendMouse(pt, flags);
}

void Input::KeepAlive() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!contacts_.empty() && GetTickCount64() - lastTouchInject_ > 50)
        InjectContacts(kMaxContacts, 0);
}
