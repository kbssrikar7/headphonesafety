#include "Settings.h"

#include <windows.h>

namespace hps {
namespace {

constexpr wchar_t kSubKey[] = L"Software\\HeadphoneSafety";

DWORD ReadDword(const wchar_t* name, DWORD defaultValue) {
    DWORD value = defaultValue;
    DWORD size = sizeof(value);
    DWORD type = REG_DWORD;
    LONG result = RegGetValueW(HKEY_CURRENT_USER, kSubKey, name, RRF_RT_REG_DWORD, &type, &value,
                                &size);
    return (result == ERROR_SUCCESS) ? value : defaultValue;
}

void WriteDword(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSubKey, name, REG_DWORD, &value, sizeof(value));
}

}  // namespace

Settings Settings::LoadOrDefault() {
    Settings s;
    s.volumeCapEnabled = ReadDword(L"VolumeCapEnabled", 1) != 0;
    s.headroomDb = static_cast<double>(ReadDword(L"HeadroomDb", 10));
    s.limiterEnabled = ReadDword(L"LimiterEnabled", 0) != 0;
    return s;
}

void Settings::Save() const {
    WriteDword(L"VolumeCapEnabled", volumeCapEnabled ? 1 : 0);
    WriteDword(L"HeadroomDb", static_cast<DWORD>(headroomDb));
    WriteDword(L"LimiterEnabled", limiterEnabled ? 1 : 0);
}

}  // namespace hps
