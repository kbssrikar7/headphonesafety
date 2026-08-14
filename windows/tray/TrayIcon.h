// Shell_NotifyIcon-based system tray icon + context menu, mirroring linux/src/tray.rs's menu
// structure: device status line, status line, separator, Volume Cap checkbox, Real-Time Limiter
// checkbox, separator, 5-item headroom radio group, separator, Quit.
//
// Owns a hidden message-only window (Shell_NotifyIcon needs a real HWND to receive callback
// messages, but the window itself is never shown). Must be constructed and used only from the
// thread that will pump its messages (main.cpp's message loop) - Win32 windows are thread-affine.
//
// Does NOT own the Settings/SharedStateServer instances it mutates - the caller (main.cpp) owns
// both and must keep them alive for at least as long as this object. Settings is also read every
// ~350ms by a separate background polling thread (see main.cpp); this class writes to it only in
// response to menu clicks, which happen on this object's own thread (the UI thread). There is no
// lock between the two threads - see main.cpp's comment at the point where the Settings pointer
// is handed to the background thread for why this is an accepted, benign race (same reasoning
// hps_shared_state.h already applies to its own fields: aligned bool/double reads/writes don't
// tear on x86/x64, and a one-tick-stale value is harmless here, not a correctness bug).
#pragma once

#include <windows.h>
#include <shellapi.h>

#include "Settings.h"
#include "SharedStateServer.h"

namespace hps {

class TrayIcon {
public:
    // hInstance: the module handle for window class registration (from wWinMain's first
    // parameter). settings/sharedState: see the class comment above on ownership.
    TrayIcon(HINSTANCE hInstance, Settings* settings, SharedStateServer* sharedState);
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool IsValid() const { return hwnd_ != nullptr; }

private:
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void AddIcon();
    void RemoveIcon();
    void ShowContextMenu();
    HMENU BuildMenu() const;

    // Persists the current in-memory Settings and, for fields the background thread's
    // SharedStateServer push loop doesn't already cover promptly enough (limiter enabled/headroom
    // - the UI should feel instant, not wait up to 350ms for the next poll tick), pushes them into
    // shared state immediately too. Volume Cap's enabled flag doesn't need an immediate shared-
    // state push - only the limiter path reads shared state.
    void PersistAndPush();

    HWND hwnd_;
    Settings* settings_;
    SharedStateServer* sharedState_;
    UINT taskbarCreatedMsg_;
    UINT iconId_;

    static constexpr UINT kTrayCallbackMessage = WM_APP + 1;
    static constexpr wchar_t kWindowClassName[] = L"HeadphoneSafetyTrayWndClass";
};

}  // namespace hps
