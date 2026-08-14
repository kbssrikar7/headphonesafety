// Runs a unit of work on a dedicated worker thread and waits up to a timeout for it to finish.
//
// Hard-won lesson #3 (docs/windows-port.md, from the macOS build): a blocking Win32/COM call
// against a device that is mid-disconnect can hang the calling thread indefinitely. You cannot
// safely TerminateThread a thread stuck inside a COM call (can corrupt COM's internal state,
// leak critical sections) - this class never attempts that. If a call times out, the worker
// thread is abandoned (left running in the background, in case it eventually returns on its
// own) and the object refuses to start a new worker until the abandoned one actually finishes,
// so at most one worker thread is ever alive per TimeoutRunner instance. The caller's job is to
// keep making progress on its own cadence (e.g. an outer poll loop) regardless of whether Run()
// succeeds or times out - never block waiting for a stuck call to resolve.
#pragma once

#include <windows.h>
#include <functional>

namespace hps {

class TimeoutRunner {
public:
    TimeoutRunner();
    ~TimeoutRunner();

    TimeoutRunner(const TimeoutRunner&) = delete;
    TimeoutRunner& operator=(const TimeoutRunner&) = delete;

    // Returns true if `work` completed within timeoutMs. Returns false either because it timed
    // out, or because a PREVIOUS call's worker thread is still stuck and hasn't finished yet - in
    // that case `work` for THIS call never even started. Check IsPreviousCallStuck() to tell the
    // two apart if it matters to the caller.
    bool Run(const std::function<void()>& work, DWORD timeoutMs);

    // True if a previously-abandoned worker thread is still outstanding (the last hang has not
    // resolved). While true, Run() will not start new work.
    bool IsPreviousCallStuck() const { return workerThread_ != nullptr; }

private:
    struct WorkerContext {
        std::function<void()> work;
    };
    static DWORD WINAPI ThreadProc(LPVOID param);

    HANDLE workerThread_;  // handle to the currently-outstanding worker thread, or nullptr
};

}  // namespace hps
