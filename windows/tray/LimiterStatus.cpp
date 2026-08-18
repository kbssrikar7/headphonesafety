#include "LimiterStatus.h"

namespace hps {

void LimiterStatus::Set(bool limiting, const std::wstring& protectedDeviceName) {
    std::lock_guard<std::mutex> lock(mutex_);
    limiting_ = limiting;
    protectedDeviceName_ = protectedDeviceName;
    blockedReason_.clear();
}

void LimiterStatus::Get(bool& limiting, std::wstring& protectedDeviceName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    limiting = limiting_;
    protectedDeviceName = protectedDeviceName_;
}

void LimiterStatus::SetBlockedReason(const std::wstring& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    blockedReason_ = reason;
}

std::wstring LimiterStatus::GetBlockedReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blockedReason_;
}

}  // namespace hps
