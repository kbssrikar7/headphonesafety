// Standalone synthetic test for TimeoutRunner - no COM, no audio, no elevated action needed.
// Confirms Run() actually returns "timed out" around the expected time rather than blocking for
// the full duration of a deliberately slow/blocking lambda, and that a second call while the
// first is still stuck refuses to start new work rather than piling up worker threads.
#include "../TimeoutRunner.h"

#include <cstdio>
#include <windows.h>

namespace {

double ElapsedMs(const LARGE_INTEGER& start, const LARGE_INTEGER& end, const LARGE_INTEGER& freq) {
    return (end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
}

}  // namespace

int main() {
    bool allPass = true;
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);

    printf("TimeoutRunner offline test\n");

    // Case 1: fast work completes within timeout.
    {
        hps::TimeoutRunner runner;
        QueryPerformanceCounter(&start);
        bool completed = runner.Run([]() { Sleep(10); }, 2000);
        QueryPerformanceCounter(&end);
        double elapsedMs = ElapsedMs(start, end, freq);
        bool pass = completed && elapsedMs < 500.0;
        printf("  [fast work, 2000ms timeout] completed=%d elapsed=%.1fms -> %s\n", completed,
               elapsedMs, pass ? "PASS" : "FAIL");
        allPass = allPass && pass;
    }

    // Case 2: slow/blocking work (5000ms) with a 1000ms timeout - Run() must return false at
    // approximately 1000ms, NOT block for the full 5000ms.
    {
        hps::TimeoutRunner runner;
        QueryPerformanceCounter(&start);
        bool completed = runner.Run([]() { Sleep(5000); }, 1000);
        QueryPerformanceCounter(&end);
        double elapsedMs = ElapsedMs(start, end, freq);
        // Generous slack for timer granularity/scheduling jitter, but must be nowhere near 5000ms.
        bool pass = !completed && elapsedMs >= 900.0 && elapsedMs < 3000.0;
        printf("  [slow work 5000ms, 1000ms timeout] completed=%d elapsed=%.1fms -> %s\n",
               completed, elapsedMs, pass ? "PASS" : "FAIL");
        allPass = allPass && pass;

        bool stillStuck = runner.IsPreviousCallStuck();
        printf("  [abandoned worker tracked as stuck] stuck=%d -> %s\n", stillStuck,
               stillStuck ? "PASS" : "FAIL");
        allPass = allPass && stillStuck;

        // A second Run() call while the first is still stuck must refuse to start new work
        // (return false immediately, without waiting the full timeout again).
        QueryPerformanceCounter(&start);
        bool secondCompleted = runner.Run([]() { Sleep(10); }, 2000);
        QueryPerformanceCounter(&end);
        double secondElapsedMs = ElapsedMs(start, end, freq);
        bool secondPass = !secondCompleted && secondElapsedMs < 200.0;
        printf("  [second call while first still stuck] completed=%d elapsed=%.1fms -> %s\n",
               secondCompleted, secondElapsedMs, secondPass ? "PASS" : "FAIL");
        allPass = allPass && secondPass;

        // Let the abandoned thread actually finish (already ~1000ms into its 5000ms sleep) before
        // this test process exits, so the test doesn't leave a background thread outliving it.
        Sleep(4500);
    }

    printf("\n%s\n", allPass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return allPass ? 0 : 1;
}
