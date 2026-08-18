// loopback_capture: a throwaway diagnostic tool, NOT part of the shipped app. Captures N seconds
// of a render device's loopback output (via WASAPI's AUDCLNT_STREAMFLAGS_LOOPBACK,
// see https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording) to a WAV file,
// so a separately-played test tone's actual peak level at the hardware can be measured (e.g. via
// `ffmpeg -i captured.wav -af volumedetect -f null -`) to verify the Real-Time Limiter is
// actually reducing gain, not just passing audio through unmodified.
//
// Usage: loopback_capture.exe <output.wav> <durationSeconds> [deviceId]
//
// deviceId (optional): an endpoint ID string exactly as IMMDevice::GetId() returns it. Needed for
// Approach B (WASAPI loopback + VB-Cable) measurements: while the Real-Time Limiter is active,
// the OS DEFAULT render device is VB-Cable itself (the limiter's own capture source), so
// capturing "the default" would just capture the unprocessed signal being fed INTO the limiter,
// not the actual processed signal LimiterEngine renders out to the real device. Pass the real
// device's ID explicitly to capture the true post-limiter output. Omit to capture the default
// device, as before (correct for a plain pass-through/no-limiter baseline).
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <cstdio>
#include <cstdint>
#include <vector>

#pragma pack(push, 1)
struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t chunkSize;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat;  // 3 = IEEE float
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize;
};
#pragma pack(pop)

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr, L"Usage: %s <output.wav> <durationSeconds>\n", argv[0]);
        return 1;
    }
    const wchar_t* outPath = argv[1];
    double durationSeconds = _wtof(argv[2]);
    if (durationSeconds <= 0) {
        fwprintf(stderr, L"durationSeconds must be > 0\n");
        return 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        fwprintf(stderr, L"CoInitializeEx failed: 0x%08x\n", hr);
        return 1;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        fwprintf(stderr, L"CoCreateInstance(MMDeviceEnumerator) failed: 0x%08x\n", hr);
        return 1;
    }

    IMMDevice* device = nullptr;
    if (argc >= 4) {
        hr = enumerator->GetDevice(argv[3], &device);
        if (FAILED(hr)) {
            fwprintf(stderr, L"GetDevice(%ls) failed: 0x%08x\n", argv[3], hr);
            enumerator->Release();
            return 1;
        }
        wprintf(L"Capturing explicit device: %ls\n", argv[3]);
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            fwprintf(stderr, L"GetDefaultAudioEndpoint failed: 0x%08x\n", hr);
            enumerator->Release();
            return 1;
        }
    }
    enumerator->Release();

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&audioClient));
    device->Release();
    if (FAILED(hr)) {
        fwprintf(stderr, L"IMMDevice::Activate(IAudioClient) failed: 0x%08x\n", hr);
        return 1;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr)) {
        fwprintf(stderr, L"GetMixFormat failed: 0x%08x\n", hr);
        return 1;
    }
    wprintf(L"Capturing loopback: %u Hz, %u channels, %u bits, format tag %u\n",
            mixFormat->nSamplesPerSec, mixFormat->nChannels, mixFormat->wBitsPerSample,
            mixFormat->wFormatTag);

    // 200ms buffer - generous, avoids overflow between our poll iterations below.
    REFERENCE_TIME bufferDuration = 200 * 10000;  // 100ns units
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  bufferDuration, 0, mixFormat, nullptr);
    if (FAILED(hr)) {
        fwprintf(stderr, L"IAudioClient::Initialize(LOOPBACK) failed: 0x%08x\n", hr);
        return 1;
    }

    IAudioCaptureClient* captureClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(&captureClient));
    if (FAILED(hr)) {
        fwprintf(stderr, L"GetService(IAudioCaptureClient) failed: 0x%08x\n", hr);
        return 1;
    }

    // WAVEFORMATEXTENSIBLE with subformat IEEE_FLOAT is what shared-mode mix formats normally are
    // on modern Windows - detect this rather than assuming, since the WAV header we write depends
    // on it (audioFormat=3 for float, =1 for PCM).
    bool isFloat = (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
        isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    if (!isFloat) {
        fwprintf(stderr, L"Warning: mix format is not IEEE float - WAV header may be wrong\n");
    }

    std::vector<BYTE> captured;
    captured.reserve(static_cast<size_t>(mixFormat->nAvgBytesPerSec * (durationSeconds + 1)));

    hr = audioClient->Start();
    if (FAILED(hr)) {
        fwprintf(stderr, L"IAudioClient::Start failed: 0x%08x\n", hr);
        return 1;
    }

    ULONGLONG startTick = GetTickCount64();
    ULONGLONG endTick = startTick + static_cast<ULONGLONG>(durationSeconds * 1000);
    while (GetTickCount64() < endTick) {
        Sleep(20);
        UINT32 packetLength = 0;
        hr = captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        while (packetLength != 0) {
            BYTE* data = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;
            hr = captureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            size_t bytesThisPacket =
                static_cast<size_t>(numFramesAvailable) * mixFormat->nBlockAlign;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                captured.insert(captured.end(), bytesThisPacket, 0);
            } else {
                captured.insert(captured.end(), data, data + bytesThisPacket);
            }

            captureClient->ReleaseBuffer(numFramesAvailable);
            hr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }
    }

    audioClient->Stop();
    captureClient->Release();
    audioClient->Release();

    wprintf(L"Captured %zu bytes (%.2f seconds)\n", captured.size(),
            static_cast<double>(captured.size()) / mixFormat->nAvgBytesPerSec);

    FILE* f = nullptr;
    if (_wfopen_s(&f, outPath, L"wb") != 0 || !f) {
        fwprintf(stderr, L"Could not open %s for writing\n", outPath);
        return 1;
    }

    WavHeader header;
    header.audioFormat = isFloat ? 3 : 1;
    header.numChannels = mixFormat->nChannels;
    header.sampleRate = mixFormat->nSamplesPerSec;
    header.bitsPerSample = mixFormat->wBitsPerSample;
    header.blockAlign = mixFormat->nBlockAlign;
    header.byteRate = mixFormat->nAvgBytesPerSec;
    header.dataSize = static_cast<uint32_t>(captured.size());
    header.chunkSize = sizeof(WavHeader) - 8 + header.dataSize;

    fwrite(&header, sizeof(header), 1, f);
    fwrite(captured.data(), 1, captured.size(), f);
    fclose(f);

    wprintf(L"Wrote %s\n", outPath);

    CoTaskMemFree(mixFormat);
    CoUninitialize();
    return 0;
}
