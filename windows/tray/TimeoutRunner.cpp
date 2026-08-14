#include "TimeoutRunner.h"

namespace hps {

TimeoutRunner::TimeoutRunner() : workerThread_(nullptr) {}

TimeoutRunner::~TimeoutRunner() {
    // Never TerminateThread here either, for the same reason as in Run() - if a worker is still
    // outstanding at destruction time, just release our handle to it (does not stop the thread;
    // it keeps running until it finishes on its own, or the process exits, whichever is first).
    if (workerThread_) {
        CloseHandle(workerThread_);
    }
}

DWORD WINAPI TimeoutRunner::ThreadProc(LPVOID param) {
    // The context (and its captured std::function) is only freed after work() returns - safe
    // even if the caller already abandoned this thread by returning from Run() on a timeout.
    auto* ctx = reinterpret_cast<WorkerContext*>(param);
    ctx->work();
    delete ctx;
    return 0;
}

bool TimeoutRunner::Run(const std::function<void()>& work, DWORD timeoutMs) {
    if (workerThread_) {
        // Non-blocking check: has a previously-abandoned worker since finished on its own?
        DWORD waitResult = WaitForSingleObject(workerThread_, 0);
        if (waitResult == WAIT_OBJECT_0) {
            CloseHandle(workerThread_);
            workerThread_ = nullptr;
        } else {
            return false;  // still stuck - refuse to pile a second worker on top of it
        }
    }

    auto* ctx = new WorkerContext{work};
    HANDLE thread = CreateThread(nullptr, 0, &TimeoutRunner::ThreadProc, ctx, 0, nullptr);
    if (!thread) {
        delete ctx;
        return false;
    }

    DWORD waitResult = WaitForSingleObject(thread, timeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        CloseHandle(thread);
        return true;
    }

    // Timed out - abandon the thread (never TerminateThread; see class header comment). Keep the
    // handle so a future Run() call can detect if/when it eventually finishes.
    workerThread_ = thread;
    return false;
}

}  // namespace hps
