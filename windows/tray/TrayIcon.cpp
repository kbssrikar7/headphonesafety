#include "TrayIcon.h"

#include "VolumeCap.h"

namespace hps {
namespace {

// Command IDs for interactive menu items. 0 is used for disabled/informational items (device
// name, status line) - never selectable, so the exact value doesn't matter beyond "not a real
// command ID."
constexpr UINT_PTR kCmdVolumeCap = 1001;
constexpr UINT_PTR kCmdLimiter = 1002;
constexpr UINT_PTR kCmdHeadroomBase = 1010;  // 1010..1014 for kHeadroomPresets[0..4]
constexpr UINT_PTR kCmdQuit = 1099;

void AppendRadioItem(HMENU menu, UINT_PTR id, const wchar_t* text, bool checked) {
    AppendMenuW(menu, MF_STRING, id, text);
    // AppendMenuW alone can't create a radio-styled (bullet, not checkmark) item - that requires
    // SetMenuItemInfoW with MFT_RADIOCHECK after the item exists. Referenced by command ID (the
    // FALSE 3rd argument), not position, so item order above doesn't need to match here.
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_FTYPE | MIIM_STATE;
    mii.fType = MFT_RADIOCHECK;
    mii.fState = checked ? MFS_CHECKED : MFS_UNCHECKED;
    SetMenuItemInfoW(menu, static_cast<UINT>(id), FALSE, &mii);
}

// Describes what Volume Cap is doing right now, without applying any clamp itself - EnforceVolumeCap
// has a side effect (it writes the device volume), which would be a surprising thing for merely
// opening a menu to trigger. This is read-only.
std::wstring DescribeVolumeCapStatus(double headroomDb) {
    DeviceSnapshot device;
    float currentDb = 0.0f, minDb = 0.0f, maxDb = 0.0f;
    if (!GetDefaultRenderDeviceSnapshot(device, currentDb, minDb, maxDb) || !device.valid) {
        return L"No output device";
    }
    bool isHeadphoneLike =
        device.kind == DeviceKind::BuiltInHeadphones || device.kind == DeviceKind::Bluetooth;
    if (!isHeadphoneLike) {
        return L"Speakers (not capped)";
    }
    float ceiling = static_cast<float>((std::max)(static_cast<double>(minDb), maxDb - headroomDb));
    return (currentDb > ceiling) ? L"Clamping active" : L"Within limit";
}

std::wstring DescribeCurrentOutput() {
    DeviceSnapshot device;
    float currentDb = 0.0f, minDb = 0.0f, maxDb = 0.0f;
    if (!GetDefaultRenderDeviceSnapshot(device, currentDb, minDb, maxDb) || !device.valid) {
        return L"Current output: unknown";
    }
    return L"Current output: " + device.friendlyName;
}

}  // namespace

TrayIcon::TrayIcon(HINSTANCE hInstance, Settings* settings, SharedStateServer* sharedState)
    : hwnd_(nullptr),
      settings_(settings),
      sharedState_(sharedState),
      taskbarCreatedMsg_(0),
      iconId_(1) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &TrayIcon::WndProcStatic;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    // RegisterClassW fails (harmlessly) if a previous instance in this same process already
    // registered the class - only one TrayIcon is ever constructed per process today, but this
    // makes that assumption non-fatal rather than silently relying on it.
    RegisterClassW(&wc);

    // A message-only window (HWND_MESSAGE parent): receives messages but can never be shown,
    // has no taskbar/Alt-Tab presence, and needs no ShowWindow call to stay invisible - simpler
    // and more explicitly "not a real window" than creating an ordinary hidden top-level window.
    hwnd_ = CreateWindowExW(0, kWindowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                             hInstance, this);
    if (!hwnd_) {
        return;
    }

    // Registered (not WM_APP-based) message: fires when explorer.exe restarts/crashes and rebuilds
    // the taskbar, which silently drops any tray icons that existed before - the standard,
    // documented fix is to listen for this and re-add the icon. A real, known Windows behavior,
    // not a hypothetical edge case.
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    AddIcon();
}

TrayIcon::~TrayIcon() {
    RemoveIcon();
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

void TrayIcon::AddIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = iconId_;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMessage;
    // Placeholder icon - a stock system icon, not a custom-designed .ico/resource. Intentional for
    // this phase (functional UI first); swap for a real branded icon in a later cosmetic pass.
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(nid.szTip, L"Headphone Safety", _TRUNCATE);

    Shell_NotifyIconW(NIM_ADD, &nid);
}

void TrayIcon::RemoveIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = iconId_;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

HMENU TrayIcon::BuildMenu() const {
    HMENU menu = CreatePopupMenu();
    if (!menu) return nullptr;

    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, DescribeCurrentOutput().c_str());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                DescribeVolumeCapStatus(settings_->headroomDb).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (settings_->volumeCapEnabled ? MF_CHECKED : MF_UNCHECKED),
                kCmdVolumeCap, L"Enable Volume Cap");
    AppendMenuW(menu, MF_STRING | (settings_->limiterEnabled ? MF_CHECKED : MF_UNCHECKED),
                kCmdLimiter, L"Enable Real-Time Limiter");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    for (int i = 0; i < kHeadroomPresetCount; ++i) {
        wchar_t label[32];
        swprintf_s(label, L"%.0f dB headroom", kHeadroomPresets[i]);
        bool checked = (kHeadroomPresets[i] == settings_->headroomDb);
        AppendRadioItem(menu, kCmdHeadroomBase + static_cast<UINT_PTR>(i), label, checked);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, kCmdQuit, L"Quit Headphone Safety");
    return menu;
}

void TrayIcon::PersistAndPush() {
    settings_->Save();
    // The limiter enabled/headroom shared-memory push already happens every ~350ms from
    // main.cpp's background loop - pushed again here too so a menu click takes effect
    // immediately rather than waiting out up to one poll cycle, matching the instant-feeling
    // toggle a user expects from clicking a checkbox.
    sharedState_->SetLimiterEnabled(settings_->limiterEnabled);
    sharedState_->SetHeadroomDb(settings_->headroomDb);
}

void TrayIcon::ShowContextMenu() {
    HMENU menu = BuildMenu();
    if (!menu) return;

    POINT pt;
    GetCursorPos(&pt);

    // Required so the popup menu dismisses correctly when the user clicks away from it - a
    // well-documented Win32 quirk (without this, the menu can get stuck open, or a second click
    // elsewhere is needed to close it). Must be called before TrackPopupMenu.
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    // Documented companion workaround to the SetForegroundWindow call above - without this dummy
    // message, the menu can fail to close in some cases (e.g. clicking a taskbar button instead
    // of anywhere else on the desktop). See Microsoft's own NOTIFYICONDATA sample code for this
    // exact pattern.
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

LRESULT CALLBACK TrayIcon::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TrayIcon* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<TrayIcon*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TrayIcon::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {
        AddIcon();
        return 0;
    }

    switch (msg) {
        case kTrayCallbackMessage:
            // Classic (non-versioned) NOTIFYICONDATA behavior: lParam IS the mouse message
            // itself (e.g. WM_RBUTTONUP), not a packed coordinate - GetCursorPos in
            // ShowContextMenu() is how the click position is obtained under this mode.
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                ShowContextMenu();
            }
            return 0;

        case WM_COMMAND: {
            UINT_PTR id = LOWORD(wParam);
            if (id == kCmdVolumeCap) {
                settings_->volumeCapEnabled = !settings_->volumeCapEnabled;
                PersistAndPush();
            } else if (id == kCmdLimiter) {
                settings_->limiterEnabled = !settings_->limiterEnabled;
                PersistAndPush();
            } else if (id >= kCmdHeadroomBase &&
                       id < kCmdHeadroomBase + static_cast<UINT_PTR>(kHeadroomPresetCount)) {
                settings_->headroomDb = kHeadroomPresets[id - kCmdHeadroomBase];
                PersistAndPush();
            } else if (id == kCmdQuit) {
                PostQuitMessage(0);
            }
            return 0;
        }

        case WM_DESTROY:
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace hps
