// Thread-safe holder for the Real-Time Limiter's live state, written by the background thread
// (main.cpp's BackgroundThreadProc, one writer) and read by TrayIcon on the UI thread when
// building the context menu (one reader, on demand - not polled). Unlike Settings' plain
// bool/double fields (deliberately left unsynchronized between these same two threads - see
// main.cpp's comment on that accepted race), this holds a std::wstring, which is NOT safe to
// read/write across threads without a lock: a string's internal buffer pointer/length can tear
// mid-read, unlike a native-width scalar.
#pragma once

#include <mutex>
#include <string>

namespace hps {

class LimiterStatus {
public:
    // Sets a definitive state (limiting or explicitly not limiting for no specific reported
    // reason) and clears any previously-set blocked reason - see SetBlockedReason.
    void Set(bool limiting, const std::wstring& protectedDeviceName);
    void Get(bool& limiting, std::wstring& protectedDeviceName) const;

    // Records a human-readable reason the limiter is enabled but NOT currently running (e.g. "the
    // Bluetooth device is in call/voice mode"), for TrayIcon to show instead of a generic
    // "starting..." - the user explicitly should not be left guessing whether this is a working
    // feature or a silent no-op. Does not itself change the limiting_ state (the caller is
    // expected to still be in the not-limiting state when it calls this).
    void SetBlockedReason(const std::wstring& reason);
    std::wstring GetBlockedReason() const;

private:
    mutable std::mutex mutex_;
    bool limiting_ = false;
    std::wstring protectedDeviceName_;
    std::wstring blockedReason_;
};

}  // namespace hps
