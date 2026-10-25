#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace valhalla {
namespace loki {

enum class BuildStatus { pending, running, success, failed };

inline std::string build_status_str(BuildStatus s) {
  switch (s) {
    case BuildStatus::pending: return "pending";
    case BuildStatus::running: return "running";
    case BuildStatus::success: return "success";
    case BuildStatus::failed:  return "failed";
  }
  return "unknown";
}

struct BuildJob {
  std::string id;
  std::string type;    // "global" or "org"
  std::string org_id;  // empty for global
  BuildStatus status = BuildStatus::pending;
  std::string error;   // non-empty on failure
  std::chrono::system_clock::time_point created_at;
  std::chrono::system_clock::time_point started_at;
  std::chrono::system_clock::time_point finished_at;

  // Seconds since epoch helpers for JSON serialisation
  long created_ts() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
        created_at.time_since_epoch()).count();
  }
  long started_ts() const {
    return status == BuildStatus::pending ? 0 :
        std::chrono::duration_cast<std::chrono::seconds>(
            started_at.time_since_epoch()).count();
  }
  long finished_ts() const {
    return (status == BuildStatus::success || status == BuildStatus::failed) ?
        std::chrono::duration_cast<std::chrono::seconds>(
            finished_at.time_since_epoch()).count() : 0;
  }
};

class BuildRegistry {
public:
  static BuildRegistry& instance() {
    static BuildRegistry inst;
    return inst;
  }

  // Submit a new job and return its ID
  std::string submit(const std::string& type, const std::string& org_id = "") {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string id = make_id();
    BuildJob job;
    job.id         = id;
    job.type       = type;
    job.org_id     = org_id;
    job.status     = BuildStatus::pending;
    job.created_at = std::chrono::system_clock::now();
    jobs_[id]      = std::move(job);
    order_.push_back(id);
    return id;
  }

  void start(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    it->second.status     = BuildStatus::running;
    it->second.started_at = std::chrono::system_clock::now();
  }

  void finish(const std::string& id, const std::string& error = "") {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    it->second.status      = error.empty() ? BuildStatus::success : BuildStatus::failed;
    it->second.error       = error;
    it->second.finished_at = std::chrono::system_clock::now();
  }

  // Returns a copy; empty id field means not found
  BuildJob get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    return it != jobs_.end() ? it->second : BuildJob{};
  }

  std::vector<BuildJob> list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BuildJob> result;
    // Return in submission order, newest first
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
      auto jit = jobs_.find(*it);
      if (jit != jobs_.end()) result.push_back(jit->second);
    }
    return result;
  }

  // Remove finished jobs older than max_age_seconds; returns count pruned
  int prune(int max_age_seconds = 3600) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = std::chrono::system_clock::now()
        - std::chrono::seconds(max_age_seconds);
    int count = 0;
    for (auto it = order_.begin(); it != order_.end(); ) {
      auto jit = jobs_.find(*it);
      if (jit != jobs_.end()) {
        const auto& job = jit->second;
        if ((job.status == BuildStatus::success || job.status == BuildStatus::failed)
            && job.finished_at < cutoff) {
          jobs_.erase(jit);
          it = order_.erase(it);
          ++count;
          continue;
        }
      }
      ++it;
    }
    return count;
  }

  // True if any job is currently pending or running
  bool any_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, job] : jobs_) {
      if (job.status == BuildStatus::pending || job.status == BuildStatus::running)
        return true;
    }
    return false;
  }

private:
  BuildRegistry() = default;
  mutable std::mutex mutex_;
  std::map<std::string, BuildJob> jobs_;
  std::vector<std::string> order_;

  static std::string make_id() {
    // Simple monotonic ID: timestamp_ms + counter
    static std::atomic<uint64_t> counter{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(ms) + "-" + std::to_string(++counter);
  }
};

// Shim for status_action.cc compat -- wraps BuildRegistry
struct ActiveBuildState {
  bool active = false;
  std::string type;
  std::string org_id;
  std::chrono::system_clock::time_point started_at;
};

struct ActiveBuild {
  static ActiveBuild& instance() {
    static ActiveBuild inst;
    return inst;
  }

  ActiveBuildState get() const {
    ActiveBuildState s;
    const auto jobs = BuildRegistry::instance().list();
    for (const auto& job : jobs) {
      if (job.status == BuildStatus::pending || job.status == BuildStatus::running) {
        s.active     = true;
        s.type       = job.type;
        s.org_id     = job.org_id;
        s.started_at = job.started_at;
        break;
      }
    }
    return s;
  }
};

} // namespace loki
} // namespace valhalla
