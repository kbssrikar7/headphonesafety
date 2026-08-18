// Owns the WASAPI capture (from VB-Cable, via loopback) + limiter + render (to the real output
// device) pipeline on a dedicated thread. Analogous in role to macOS's LimiterEngine.swift, but
// structurally simpler: WASAPI capture and render can share one thread (see LimiterEngine.cpp's
// header comment for why), unlike CoreAudio's three independently-scheduled Audio Units.
//
// Does NOT touch the OS default output device itself - that is the caller's responsibility
// (DefaultDeviceSwitcher), and must happen in the right order relative to Start()/Stop() (see
// main.cpp's lifecycle code and its comments on why the ordering matters - a real bug class,
// not caution for its own sake).
#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>

#include "Limiter.h"

namespace hps {

class LimiterEngine {
public:
    using FailureCallback = std::function<void()>;

    LimiterEngine();
    ~LimiterEngine();

    LimiterEngine(const LimiterEngine&) = delete;
    LimiterEngine& operator=(const LimiterEngine&) = delete;

    // realDeviceId: the real output device's endpoint ID (render target). vbCableId: VB-Cable's
    // endpoint ID (capture source, via loopback) - by the time this is called, the OS default
    // output must already be switched to vbCableId (see the class comment above). All WASAPI
    // object creation happens on the dedicated thread itself (COM objects are best used on the
    // thread that creates them); Start() blocks (with a bounded timeout) until that thread's
    // setup phase reports success or failure, so callers get a synchronous true/false the same
    // way the old shared-memory-only design implicitly did. Returns false if setup failed (device
    // activation, format negotiation, or stream initialization error, or a timeout waiting for
    // setup) - caller should treat this as a failed Start() and revert.
    bool Start(const std::wstring& realDeviceId, const std::wstring& vbCableId, double headroomDb);

    // Stops the thread and releases all WASAPI resources. Safe to call even if not running.
    void Stop();

    // Real-time-adjacent-safe: just an atomic store, no allocation/locking, cheap enough to call
    // from the poll loop on every settings change.
    void UpdateHeadroomDb(double headroomDb);

    bool IsRunning() const;

    // Called from the engine's own thread if a capture/render error occurs mid-stream (e.g. a
    // device was invalidated). The callback runs ON the engine thread - it must not call back
    // into Stop() synchronously (would deadlock joining its own thread); post a message/set a
    // flag for another thread to react to, matching how DeviceWatcher's callbacks work.
    void SetFailureCallback(FailureCallback cb);

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void Run();

    HANDLE thread_;
    HANDLE stopEvent_;
    HANDLE setupCompleteEvent_;  // signaled by the thread once setup succeeds or fails
    std::atomic<bool> setupSucceeded_;
    std::atomic<bool> running_;
    std::atomic<double> headroomDb_;
    std::wstring realDeviceId_;
    std::wstring vbCableId_;
    FailureCallback onFailure_;
};

}  // namespace hps
