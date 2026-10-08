#pragma once

#include <chrono>
#include <mutex>
#include <string>

namespace valhalla {
namespace loki {

struct BuildState {
  bool active = false;
  std::string type;      // "global" or "org"
  std::string org_id;    // empty for global
  std::chrono::system_clock::time_point started_at;
};

// Singleton shared between admin_action.cc and status_action.cc
class ActiveBuild {
public:
  static ActiveBuild& instance() {
    static ActiveBuild inst;
    return inst;
  }

  void start(const std::string& type, const std::string& org_id = "") {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active     = true;
    state_.type       = type;
    state_.org_id     = org_id;
    state_.started_at = std::chrono::system_clock::now();
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = BuildState{};
  }

  BuildState get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

private:
  ActiveBuild() = default;
  mutable std::mutex mutex_;
  BuildState state_;
};

} // namespace loki
} // namespace valhalla
