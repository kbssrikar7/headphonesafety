// Thin, isolated wrapper around the undocumented IPolicyConfig interface (see PolicyConfig.h) for
// switching the OS default render endpoint. Kept small and separate from LimiterEngine so the
// undocumented-COM-interface risk is contained to one file, independently testable.
//
// Hard-won lesson #1 (docs/windows-port.md): a "successful" HRESULT from an undocumented
// interface is even less trustworthy than from a documented one - every Set call here should be
// paired with a readback via GetCurrentDefaultRenderDeviceId by the caller.
#pragma once

#include <string>

namespace hps {

// Sets deviceId (the full endpoint ID string exactly as IMMDevice::GetId() returns it) as the
// default render endpoint for all three roles (eConsole, eMultimedia, eCommunications) - a
// partial switch leaves some apps routed to the wrong device silently, so this always does all
// three. Returns true only if the underlying calls reported success; the caller must still verify
// via GetCurrentDefaultRenderDeviceId, this return value alone is not proof (hard-won lesson #1).
bool SetDefaultRenderDevice(const std::wstring& deviceId);

// Returns the current default render endpoint's ID string (eConsole role - the one apps
// overwhelmingly query), or an empty string if it could not be determined.
std::wstring GetCurrentDefaultRenderDeviceId();

// Looks up an arbitrary device's friendly name by its endpoint ID string - unlike
// VolumeCap.cpp's GetDefaultRenderDeviceSnapshot, this works for a device that is NOT the
// current default (e.g. the real device being protected while the OS default is switched to
// VB-Cable). Returns an empty string if the device can't be found (e.g. unplugged).
std::wstring GetFriendlyName(const std::wstring& deviceId);

// True if deviceId's own native mix format looks like a Bluetooth Hands-Free/telephony (HFP)
// voice-call endpoint rather than an A2DP music endpoint. Windows exposes these as two entirely
// separate render endpoints for the same physical Bluetooth device (observed live: "Headphones"
// at 44100 Hz/2ch for A2DP vs "Headset" at 16000 Hz/1ch for HFP, on the same Sony WH-CH720N) - the
// OS can silently flip which one is the default (e.g. when some app requests microphone/call
// access), and protecting the HFP endpoint would mean silently "limiting" a mono voice-call
// channel instead of the music the user actually hears. Real A2DP codecs (SBC/AAC/aptX) never go
// below 32000 Hz, while HFP's CVSD/mSBC codecs run at 8000/16000 Hz, so a simple sample-rate
// threshold is a reliable, low-risk heuristic - deliberately not also gating on channel count
// (mono), since that alone is a weaker signal that could exist on legitimate hardware.
bool IsLikelyBluetoothVoiceEndpoint(const std::wstring& deviceId);

}  // namespace hps
