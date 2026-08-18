// Capture (VB-Cable, loopback, event-driven) + limiter + render (real device) on one dedicated
// thread. Unlike macOS's CoreAudio Audio-Unit pull-chain (which needed RingBuffer.swift because
// three independently-scheduled Audio Units each pull on their own cadence), WASAPI capture and
// render here share one thread: wait for the capture event -> GetBuffer (capture) ->
// Limiter::Process() in place -> GetBuffer (render) -> copy -> ReleaseBuffer both. No cross-thread
// ring buffer needed.
//
// All WASAPI/COM object creation happens on this thread, not in Start() (the caller's thread) -
// COM objects are best created and used on the thread that will drive them. Start() blocks on
// setupCompleteEvent_ so it can still report success/failure synchronously to the caller.
//
// Not as strict as an APO's real-time thread (this is ordinary user-mode WASAPI, not a
// driver-hosted APO inside audiodg.exe) but still glitch-sensitive - uses MMCSS ("Pro Audio")
// for scheduling priority, the standard approach for low-latency WASAPI apps.
#include "LimiterEngine.h"

#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#pragma comment(lib, "avrt.lib")

namespace hps {
namespace {

// 30ms capture buffer - small enough for low propagation delay; capture is event-driven
// (AUDCLNT_STREAMFLAGS_EVENTCALLBACK) so it doesn't need much slack, WASAPI wakes this thread as
// soon as a packet is ready. In 100ns units for WASAPI's REFERENCE_TIME.
constexpr REFERENCE_TIME kBufferDuration = 30 * 10000;

// 150ms render buffer - deliberately much larger than the capture buffer. Unlike capture, render
// is NOT event-driven here (no AUDCLNT_STREAMFLAGS_EVENTCALLBACK on the render client): this
// thread only refills it reactively, in response to capture events, so it has no clock of its own
// telling it when the audio engine is about to run dry. A 30ms render buffer left almost no slack
// against ordinary desktop scheduling jitter on this thread (message pump/COM/other work
// competing for CPU in the same process) - confirmed live via WASAPI loopback measurement: a
// 30ms render buffer produced audible-signature glitches (elevated crest factor, peak pinned near
// 0 dBFS even while mean/RMS was correctly attenuated toward the configured ceiling) that
// persisted even after fixing a separate frame-drop bug in the capture/render coupling, pointing
// to render-side underrun rather than dropped frames as the real cause. 150ms trades a bit more
// end-to-end latency (acceptable for a background hearing-protection limiter, not a live
// monitoring/production tool) for headroom against that jitter.
constexpr REFERENCE_TIME kRenderBufferDuration = 150 * 10000;
// Bounded wait for the engine thread to finish its setup phase - normal setup completes in low
// single-digit milliseconds; this is generous headroom, not a tuned value.
constexpr DWORD kSetupTimeoutMs = 5000;
constexpr DWORD kCaptureEventTimeoutMs = 500;

IMMDevice* OpenDeviceById(const std::wstring& deviceId) {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(&enumerator)))) {
        return nullptr;
    }
    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDevice(deviceId.c_str(), &device);
    enumerator->Release();
    return SUCCEEDED(hr) ? device : nullptr;
}

}  // namespace

LimiterEngine::LimiterEngine()
    : thread_(nullptr),
      stopEvent_(nullptr),
      setupCompleteEvent_(nullptr),
      setupSucceeded_(false),
      running_(false),
      headroomDb_(10.0) {}

LimiterEngine::~LimiterEngine() {
    Stop();
}

void LimiterEngine::SetFailureCallback(FailureCallback cb) {
    onFailure_ = std::move(cb);
}

void LimiterEngine::UpdateHeadroomDb(double headroomDb) {
    headroomDb_.store(headroomDb, std::memory_order_relaxed);
}

bool LimiterEngine::IsRunning() const {
    return running_.load(std::memory_order_relaxed);
}

bool LimiterEngine::Start(const std::wstring& realDeviceId, const std::wstring& vbCableId,
                           double headroomDb) {
    if (running_.load(std::memory_order_relaxed)) return false;

    realDeviceId_ = realDeviceId;
    vbCableId_ = vbCableId;
    headroomDb_.store(headroomDb, std::memory_order_relaxed);
    setupSucceeded_.store(false, std::memory_order_relaxed);

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    setupCompleteEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_ || !setupCompleteEvent_) {
        if (stopEvent_) CloseHandle(stopEvent_);
        if (setupCompleteEvent_) CloseHandle(setupCompleteEvent_);
        stopEvent_ = setupCompleteEvent_ = nullptr;
        return false;
    }

    thread_ = CreateThread(nullptr, 0, &LimiterEngine::ThreadProc, this, 0, nullptr);
    if (!thread_) {
        CloseHandle(stopEvent_);
        CloseHandle(setupCompleteEvent_);
        stopEvent_ = setupCompleteEvent_ = nullptr;
        return false;
    }

    DWORD waitResult = WaitForSingleObject(setupCompleteEvent_, kSetupTimeoutMs);
    bool ok = (waitResult == WAIT_OBJECT_0) && setupSucceeded_.load(std::memory_order_relaxed);
    if (!ok) {
        // Setup failed or timed out - signal stop and clean up. If it timed out with a stuck
        // WASAPI call (hard-won lesson #3's failure mode, though less likely here since these are
        // freshly-resolved devices, not ones mid-disconnect), do not join indefinitely - detach
        // by not waiting further; the thread will exit on its own if/when the stuck call
        // eventually returns, same "abandon rather than force-kill" policy as TimeoutRunner.
        SetEvent(stopEvent_);
        WaitForSingleObject(thread_, 1000);
        CloseHandle(thread_);
        CloseHandle(stopEvent_);
        CloseHandle(setupCompleteEvent_);
        thread_ = stopEvent_ = setupCompleteEvent_ = nullptr;
        running_.store(false, std::memory_order_relaxed);
        return false;
    }

    running_.store(true, std::memory_order_relaxed);
    return true;
}

void LimiterEngine::Stop() {
    if (!thread_) return;
    if (stopEvent_) SetEvent(stopEvent_);
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
    if (stopEvent_) CloseHandle(stopEvent_);
    if (setupCompleteEvent_) CloseHandle(setupCompleteEvent_);
    thread_ = stopEvent_ = setupCompleteEvent_ = nullptr;
    running_.store(false, std::memory_order_relaxed);
}

DWORD WINAPI LimiterEngine::ThreadProc(LPVOID param) {
    auto* self = reinterpret_cast<LimiterEngine*>(param);
    self->Run();
    return 0;
}

void LimiterEngine::Run() {
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInitializedHere = SUCCEEDED(comHr);

    IMMDevice* vbCableDevice = nullptr;
    IMMDevice* realDevice = nullptr;
    IAudioClient* captureClient = nullptr;
    IAudioClient* renderClient = nullptr;
    IAudioCaptureClient* captureService = nullptr;
    IAudioRenderClient* renderService = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    // renderFormat points at whichever format the render client actually got Initialize'd with:
    // either == mixFormat (capture/VB-Cable format accepted directly, no conversion needed) or ==
    // renderFormatOwned (the render device's own native format, when it rejected mixFormat - see
    // the fallback below). renderFormatOwned is a separate CoTaskMemFree-owned allocation only in
    // the latter case.
    WAVEFORMATEX* renderFormat = nullptr;
    WAVEFORMATEX* renderFormatOwned = nullptr;
    bool needsResample = false;
    HANDLE captureEvent = nullptr;
    HANDLE avrtHandle = nullptr;
    DWORD avrtTaskIndex = 0;

    auto cleanup = [&]() {
        if (renderClient) renderClient->Stop();
        if (captureClient) captureClient->Stop();
        if (renderService) renderService->Release();
        if (captureService) captureService->Release();
        if (renderClient) renderClient->Release();
        if (captureClient) captureClient->Release();
        if (realDevice) realDevice->Release();
        if (vbCableDevice) vbCableDevice->Release();
        if (mixFormat) CoTaskMemFree(mixFormat);
        if (renderFormatOwned) CoTaskMemFree(renderFormatOwned);
        if (captureEvent) CloseHandle(captureEvent);
        if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
        if (comInitializedHere) CoUninitialize();
    };

    vbCableDevice = OpenDeviceById(vbCableId_);
    realDevice = OpenDeviceById(realDeviceId_);
    if (!vbCableDevice || !realDevice) {
        fwprintf(stderr, L"[LimiterEngine] failed to open capture/render device\n");
        cleanup();
        SetEvent(setupCompleteEvent_);
        return;
    }

    if (FAILED(vbCableDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(&captureClient)))) {
        fwprintf(stderr, L"[LimiterEngine] failed to activate capture IAudioClient\n");
        cleanup();
        SetEvent(setupCompleteEvent_);
        return;
    }

    if (FAILED(captureClient->GetMixFormat(&mixFormat))) {
        fwprintf(stderr, L"[LimiterEngine] GetMixFormat failed\n");
        cleanup();
        SetEvent(setupCompleteEvent_);
        return;
    }

    if (FAILED(captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_LOOPBACK |
                                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                          kBufferDuration, 0, mixFormat, nullptr))) {
        fwprintf(stderr, L"[LimiterEngine] capture Initialize failed\n");
        cleanup();
        SetEvent(setupCompleteEvent_);
        return;
    }

    captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent || FAILED(captureClient->SetEventHandle(captureEvent))) {
        fwprintf(stderr, L"[LimiterEngine] SetEventHandle failed\n");
        cleanup();
        SetEvent(setupCompleteEvent_);
        return;
    }

    if (FAILED(captureClient->GetService(__uuidof(IAudioCaptureClient),
                                          reinterpret_cast<void**>(&captureService)))) {
        fwprintf(stderr, L"[LimiterEngine] GetService(IAudioCaptureClient) failed\n");
        cleanup();
        SetEvent(setupCompleteEvent_);
        return;
    }

    // Render side: try the capture side's mix format first (VB-Cable's format) - on a machine
    // where the render device shares the same native format as VB-Cable, this just works with no
    // conversion needed. Confirmed live: this succeeds against the built-in Conexant Speakers
    // device, but FAILS against a real Bluetooth (A2DP) headset - AUDCLNT_E_UNSUPPORTED_FORMAT -
    // because the headset's own native mix format differs from VB-Cable's. Fall back to the
    // render device's own native format + on-the-fly linear resampling/channel remixing (below)
    // rather than leaving Bluetooth headphones - a very common real-world case - unsupported.
    if (FAILED(realDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(&renderClient)))) {
        fwprintf(stderr, L"[LimiterEngine] failed to activate render IAudioClient\n");
        cleanup();
        SetEvent(setupCompleteEvent_);
        return;
    }

    HRESULT renderInitHr = renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                                      kRenderBufferDuration, 0, mixFormat, nullptr);
    if (SUCCEEDED(renderInitHr)) {
        renderFormat = mixFormat;
    } else {
        fwprintf(stderr,
                  L"[LimiterEngine] render Initialize with capture's mix format failed "
                  L"(0x%08x) - retrying with the render device's own native mix format\n",
                  renderInitHr);
        if (FAILED(renderClient->GetMixFormat(&renderFormatOwned))) {
            fwprintf(stderr, L"[LimiterEngine] render GetMixFormat failed\n");
            cleanup();
            SetEvent(setupCompleteEvent_);
            return;
        }
        bool isFloat = (renderFormatOwned->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (renderFormatOwned->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(renderFormatOwned);
            isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }
        if (!isFloat) {
            // The limiter and resampler below both assume 32-bit float samples (as does the
            // capture side's mixFormat, which is always float in shared mode) - refuse to guess
            // at a PCM integer layout rather than silently producing corrupted audio.
            fwprintf(stderr,
                      L"[LimiterEngine] render device's native format is not IEEE float - cannot "
                      L"safely convert, aborting\n");
            cleanup();
            SetEvent(setupCompleteEvent_);
            return;
        }
        renderInitHr = renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kRenderBufferDuration,
                                                 0, renderFormatOwned, nullptr);
        if (FAILED(renderInitHr)) {
            fwprintf(stderr,
                      L"[LimiterEngine] render Initialize with the render device's own native "
                      L"mix format ALSO failed (0x%08x)\n",
                      renderInitHr);
            cleanup();
            SetEvent(setupCompleteEvent_);
            return;
        }
        renderFormat = renderFormatOwned;
        needsResample = (renderFormat->nSamplesPerSec != mixFormat->nSamplesPerSec) ||
                         (renderFormat->nChannels != mixFormat->nChannels);
        fwprintf(stderr,
                  L"[LimiterEngine] render device native format: %u Hz, %u ch (capture: %u Hz, "
                  L"%u ch) - %ls\n",
                  renderFormat->nSamplesPerSec, renderFormat->nChannels, mixFormat->nSamplesPerSec,
                  mixFormat->nChannels,
                  needsResample ? L"resampling engaged" : L"formats compatible, no conversion needed");
    }

    if (FAILED(renderClient->GetService(__uuidof(IAudioRenderClient),
                                         reinterpret_cast<void**>(&renderService)))) {
        fwprintf(stderr, L"[LimiterEngine] GetService(IAudioRenderClient) failed\n");
        cleanup();
        SetEvent(setupCompleteEvent_);
        return;
    }

    UINT32 renderBufferFrameCount = 0;
    renderClient->GetBufferSize(&renderBufferFrameCount);

    // Prime the render buffer with silence before Start() - standard WASAPI shared-mode practice
    // (see Microsoft's own render-client sample code). Starting playback on a completely empty
    // buffer risks an initial underrun/glitch while this thread races to deliver the first real
    // packet; with the larger 150ms render buffer above, an empty start would also mean this
    // thread has to fill 150ms of backlog before steady-state, worsening that risk rather than
    // helping it.
    {
        BYTE* primeData = nullptr;
        if (SUCCEEDED(renderService->GetBuffer(renderBufferFrameCount, &primeData))) {
            renderService->ReleaseBuffer(renderBufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    // MMCSS priority - standard for low-latency WASAPI apps (DAWs, OBS's audio capture). Not a
    // hard requirement the way an APO's zero-allocation rule was, but reduces glitch risk under
    // system load.
    avrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &avrtTaskIndex);

    hps::Limiter limiter;
    limiter.Reset(static_cast<double>(mixFormat->nSamplesPerSec), mixFormat->nChannels);

    if (FAILED(captureClient->Start())) {
        fwprintf(stderr, L"[LimiterEngine] capture Start failed\n");
        cleanup();
        SetEvent(setupCompleteEvent_);
        return;
    }
    if (FAILED(renderClient->Start())) {
        fwprintf(stderr, L"[LimiterEngine] render Start failed\n");
        cleanup();
        SetEvent(setupCompleteEvent_);
        return;
    }

    // Setup succeeded - let Start() on the caller's thread return true now, before entering the
    // processing loop below.
    setupSucceeded_.store(true, std::memory_order_relaxed);
    SetEvent(setupCompleteEvent_);

    // FIFO between capture and render. Needed because the two streams are NOT tied together by a
    // shared clock/event - capture delivers packets on its own event cadence, and the render
    // buffer's available room depends on how fast the audio engine has actually drained it since
    // our last write, which can briefly lag behind (scheduling jitter, capture delivering a
    // slightly larger-than-usual packet, etc). The FIRST version of this loop rendered
    // min(framesAvailable, available) frames directly from the just-captured packet and then
    // unconditionally released the ENTIRE captured packet - silently discarding whatever didn't
    // fit, and only ever running the limiter over the discarded packet's rendered prefix. Confirmed
    // live via WASAPI loopback measurement: this produced a signature of clicks/discontinuities
    // (elevated crest factor, peak pinned near 0 dBFS even while mean/RMS level was correctly
    // reduced toward the configured ceiling) rather than clean, fully-attenuated audio. Buffering
    // ALL captured+processed audio here and draining only as much as the render buffer currently
    // has room for, every iteration, guarantees no audio is ever dropped for a transient mismatch.
    // pending is always in renderFormat's byte layout (== mixFormat's layout when no resampling
    // is needed, so this is a no-op change from the original single-format behavior in that case).
    std::vector<BYTE> pending;
    pending.reserve(static_cast<size_t>(renderFormat->nAvgBytesPerSec) / 2);  // ~500ms headroom

    // Pathological safety valve only (e.g. render device wedged/not draining at all) - mirrors
    // TimeoutRunner's "bounded, not infinite" philosophy elsewhere in this codebase. Normal
    // operation should never come close to this; if it's ever hit, drop the OLDEST audio (not the
    // newest) so latency doesn't grow unbounded, and log once so it's visible during testing.
    const size_t kMaxPendingBytes =
        static_cast<size_t>(renderFormat->nAvgBytesPerSec) * 2;  // 2 seconds
    bool loggedOverflow = false;

    auto drainToRender = [&]() {
        UINT32 padding = 0;
        renderClient->GetCurrentPadding(&padding);
        UINT32 available =
            (renderBufferFrameCount > padding) ? (renderBufferFrameCount - padding) : 0;
        UINT32 pendingFrames = static_cast<UINT32>(pending.size() / renderFormat->nBlockAlign);
        UINT32 framesToRender = (pendingFrames < available) ? pendingFrames : available;
        if (framesToRender == 0) return;

        BYTE* renderData = nullptr;
        HRESULT hr = renderService->GetBuffer(framesToRender, &renderData);
        if (SUCCEEDED(hr)) {
            size_t bytesToCopy = static_cast<size_t>(framesToRender) * renderFormat->nBlockAlign;
            memcpy(renderData, pending.data(), bytesToCopy);
            renderService->ReleaseBuffer(framesToRender, 0);
            pending.erase(pending.begin(), pending.begin() + bytesToCopy);
        } else {
            fwprintf(stderr, L"[LimiterEngine] render GetBuffer failed (0x%08x)\n", hr);
        }
    };

    // Resampling/remixing state - only exercised when needsResample is true (render device's
    // native format differs from VB-Cable's). Linear interpolation, continuous across packet
    // boundaries via resampleHistory (the last remixed source frame from the previous packet) and
    // resamplePos (fractional position, in source-frame units relative to that history frame).
    // Good enough for a background hearing-protection limiter - not a production resampling-
    // quality bar (no anti-aliasing filter), but the sample-rate deltas involved here (e.g.
    // 48000 vs 44100/48000 mono-vs-stereo Bluetooth codecs) are mild.
    std::vector<float> resampleHistory(renderFormat->nChannels, 0.0f);
    bool resampleHistoryValid = false;
    double resamplePos = 0.0;

    auto remixFrame = [&](const float* src, float* dst) {
        UINT32 sc = mixFormat->nChannels;
        UINT32 dc = renderFormat->nChannels;
        if (sc == dc) {
            for (UINT32 c = 0; c < dc; ++c) dst[c] = src[c];
        } else if (sc == 1 && dc == 2) {
            dst[0] = dst[1] = src[0];
        } else if (sc == 2 && dc == 1) {
            dst[0] = 0.5f * (src[0] + src[1]);
        } else if (sc < dc) {
            for (UINT32 c = 0; c < dc; ++c) dst[c] = src[c % sc];
        } else {
            float sum = 0.0f;
            for (UINT32 c = 0; c < sc; ++c) sum += src[c];
            sum /= static_cast<float>(sc);
            for (UINT32 c = 0; c < dc; ++c) dst[c] = sum;
        }
    };

    // Remixes + resamples one captured (already limiter-processed) packet from capture format
    // (mixFormat) to render format (renderFormat), appending the result directly to `pending`.
    auto resampleAndAppend = [&](const float* srcData, UINT32 srcFrames) {
        if (srcFrames == 0) return;
        UINT32 dc = renderFormat->nChannels;
        double step = static_cast<double>(mixFormat->nSamplesPerSec) /
                      static_cast<double>(renderFormat->nSamplesPerSec);

        std::vector<float> remixed(static_cast<size_t>(srcFrames + 1) * dc);
        if (!resampleHistoryValid) {
            // No history yet (first packet since Start) - seed it with this packet's own first
            // frame so playback starts cleanly instead of interpolating from silence.
            remixFrame(srcData, remixed.data());
            resampleHistoryValid = true;
        } else {
            std::copy(resampleHistory.begin(), resampleHistory.end(), remixed.begin());
        }
        for (UINT32 i = 0; i < srcFrames; ++i) {
            remixFrame(srcData + static_cast<size_t>(i) * mixFormat->nChannels,
                       remixed.data() + static_cast<size_t>(i + 1) * dc);
        }

        std::vector<float> outFrames;
        outFrames.reserve(static_cast<size_t>(srcFrames / step) + 4);
        while (resamplePos <= static_cast<double>(srcFrames)) {
            size_t i0 = static_cast<size_t>(resamplePos);
            size_t i1 = i0 + 1;
            if (i1 > srcFrames) break;
            double frac = resamplePos - static_cast<double>(i0);
            for (UINT32 c = 0; c < dc; ++c) {
                float a = remixed[i0 * dc + c];
                float b = remixed[i1 * dc + c];
                outFrames.push_back(static_cast<float>(a + (b - a) * frac));
            }
            resamplePos += step;
        }
        resamplePos -= static_cast<double>(srcFrames);
        std::copy(remixed.end() - dc, remixed.end(), resampleHistory.begin());

        if (!outFrames.empty()) {
            size_t oldSize = pending.size();
            pending.resize(oldSize + outFrames.size() * sizeof(float));
            memcpy(pending.data() + oldSize, outFrames.data(), outFrames.size() * sizeof(float));
        }
    };

    HANDLE waitHandles[2] = {stopEvent_, captureEvent};
    for (;;) {
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, kCaptureEventTimeoutMs);
        if (waitResult == WAIT_OBJECT_0) {
            // stopEvent_ signaled.
            break;
        }
        if (waitResult == WAIT_TIMEOUT) {
            // No capture activity within the timeout - not necessarily an error (silence can
            // still deliver empty/silent packets depending on the engine), but if this persists
            // it likely means the capture device stopped delivering data (e.g. invalidated).
            // Keep looping; a real invalidation surfaces via a failed GetBuffer call below, which
            // does trigger the failure callback. Still worth draining here: if the render side
            // was briefly full on a previous iteration, room may have freed up since then with no
            // new capture event to trigger another drain.
            drainToRender();
            continue;
        }
        if (waitResult != WAIT_OBJECT_0 + 1) {
            // Unexpected wait result (WAIT_FAILED or an abandoned mutex, neither expected here).
            fwprintf(stderr, L"[LimiterEngine] unexpected wait result %lu\n", waitResult);
            break;
        }

        UINT32 packetLength = 0;
        HRESULT hr = captureService->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            fwprintf(stderr, L"[LimiterEngine] GetNextPacketSize failed (0x%08x)\n", hr);
            break;
        }

        while (packetLength != 0) {
            BYTE* captureData = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;
            hr = captureService->GetBuffer(&captureData, &framesAvailable, &flags, nullptr,
                                            nullptr);
            if (FAILED(hr)) {
                fwprintf(stderr, L"[LimiterEngine] capture GetBuffer failed (0x%08x)\n", hr);
                if (onFailure_) onFailure_();
                cleanup();
                return;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                // Benign - a scheduling gap occurred; the limiter's envelope state just
                // continues from wherever it was, no special handling needed.
            }

            // Process the FULL captured packet (never just a render-buffer-limited prefix - see
            // the comment above `pending`) in a local buffer (always in capture/mixFormat layout,
            // since the limiter and resampler both expect that), then append the result to the
            // FIFO - either directly (no resampling needed) or via resampleAndAppend (render
            // device's native format differs from VB-Cable's). Copy out of captureData BEFORE
            // releasing the capture buffer, since captureData is only valid until ReleaseBuffer.
            size_t bytesThisPacket = static_cast<size_t>(framesAvailable) * mixFormat->nBlockAlign;
            std::vector<BYTE> packetBuf(bytesThisPacket);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // Silent packet - still run it through the limiter's release phase (Process
                // handles zero input naturally, envelope decays toward 0) by zeroing the region
                // directly, matching what capture delivered.
                memset(packetBuf.data(), 0, bytesThisPacket);
            } else {
                memcpy(packetBuf.data(), captureData, bytesThisPacket);
            }
            limiter.Process(reinterpret_cast<float*>(packetBuf.data()), framesAvailable,
                             mixFormat->nChannels,
                             hps::HeadroomDbToLinearCeiling(
                                 headroomDb_.load(std::memory_order_relaxed)));

            captureService->ReleaseBuffer(framesAvailable);

            if (needsResample) {
                resampleAndAppend(reinterpret_cast<const float*>(packetBuf.data()),
                                   framesAvailable);
            } else {
                size_t oldSize = pending.size();
                pending.resize(oldSize + bytesThisPacket);
                memcpy(pending.data() + oldSize, packetBuf.data(), bytesThisPacket);
            }

            if (pending.size() > kMaxPendingBytes) {
                // Pathological case only (render device not draining at all) - drop the oldest
                // excess rather than let latency/memory grow without bound.
                size_t excess = pending.size() - kMaxPendingBytes;
                excess -= excess % renderFormat->nBlockAlign;  // keep frame-aligned
                pending.erase(pending.begin(), pending.begin() + excess);
                if (!loggedOverflow) {
                    fwprintf(stderr,
                              L"[LimiterEngine] pending buffer overflow - render device not "
                              L"draining, dropping oldest audio\n");
                    loggedOverflow = true;
                }
            }

            drainToRender();

            hr = captureService->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                fwprintf(stderr, L"[LimiterEngine] GetNextPacketSize failed (0x%08x)\n", hr);
                if (onFailure_) onFailure_();
                cleanup();
                return;
            }
        }
    }

    cleanup();
}

}  // namespace hps
