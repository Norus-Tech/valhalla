#include "loki/worker.h"
#include "loki/build_state.h"
#include "midgard/logging.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <boost/property_tree/ptree.hpp>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <thread>

namespace {

// Run a shell command and return stdout. Throws on non-zero exit.
std::string shell_exec(const std::string& cmd) {
  std::array<char, 256> buf;
  std::string output;
  FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
  if (!pipe)
    throw std::runtime_error("popen failed for: " + cmd);
  while (fgets(buf.data(), buf.size(), pipe))
    output += buf.data();
  int rc = pclose(pipe);
  if (rc != 0)
    throw std::runtime_error("Command failed (rc=" + std::to_string(rc) + "): " + cmd + "\n" + output);
  return output;
}

std::string json_ok(const std::string& message) {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  w.StartObject();
  w.Key("status"); w.String("ok");
  w.Key("message"); w.String(message.c_str());
  w.EndObject();
  return sb.GetString();
}

std::string json_error(const std::string& message) {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  w.StartObject();
  w.Key("status"); w.String("error");
  w.Key("message"); w.String(message.c_str());
  w.EndObject();
  return sb.GetString();
}

// Generate a version string: {admin.app_version:-1.0.0}-{unix_seconds}
std::string make_version(const boost::property_tree::ptree& config) {
  auto now = std::chrono::system_clock::now();
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(
      now.time_since_epoch()).count();
  const std::string base = config.get<std::string>("admin.app_version", "1.0.0");
  return base + "-" + std::to_string(secs);
}

// s3_base: s3://{bucket}/downloads
// scope:   "global" | "org/{orgId}" | "region/{region}"
std::string s3_scope_prefix(const std::string& s3_base,
                             const std::string& scope) {
  return s3_base + "/" + scope;
}

void write_s3_latest(const std::string& scope_prefix,
                     const std::string& version) {
  // Write version string to a temp file, upload as latest pointer
  const std::string tmp = "/tmp/valhalla-latest.txt";
  FILE* f = fopen(tmp.c_str(), "w");
  if (f) { fputs(version.c_str(), f); fclose(f); }
  shell_exec("aws s3 cp " + tmp + " " + scope_prefix + "/latest");
}

std::string read_s3_latest(const std::string& scope_prefix) {
  const std::string tmp = "/tmp/valhalla-latest.txt";
  try {
    shell_exec("aws s3 cp " + scope_prefix + "/latest " + tmp);
  } catch (...) {
    return ""; // no latest yet
  }
  std::string version;
  FILE* f = fopen(tmp.c_str(), "r");
  if (f) {
    char buf[64];
    if (fgets(buf, sizeof(buf), f)) version = buf;
    fclose(f);
  }
  // trim whitespace
  while (!version.empty() && (version.back() == '\n' || version.back() == ' '))
    version.pop_back();
  return version;
}

// -- Sub-action handlers ------------------------------------------------------

std::string do_build(const std::string& org_id,
                     const std::string& version,
                     const boost::property_tree::ptree& config) {
  // Use isolated build config so in-progress builds don't disturb the live service
  const std::string cfg       = config.get<std::string>("admin.build_config_path",
                                   "/app/valhalla/valhalla-build.json");
  const std::string tile_dir  = config.get<std::string>("mjolnir.tile_dir",
                                   "/app/valhalla/build/tiles");
  const std::string s3_base   = config.get<std::string>("admin.s3_downloads_base", "");
  if (s3_base.empty())
    throw std::runtime_error("admin.s3_downloads_base not configured");

  // Determine scope and PBF source
  std::string scope;
  std::string pbf_path;

  if (org_id.empty()) {
    // Global build
    const std::string s3_pbf = config.get<std::string>("admin.s3_pbf_url", "");
    if (s3_pbf.empty())
      throw std::runtime_error("admin.s3_pbf_url not configured");
    scope    = "global";
    pbf_path = "/app/valhalla/build-global.osm.pbf";
    shell_exec("aws s3 cp " + s3_pbf + " " + pbf_path);
  } else {
    // Org build: API has written org-scoped PBF to S3
    scope    = "org/" + org_id;
    pbf_path = "/app/valhalla/build-" + org_id + ".osm.pbf";
    const std::string s3_input = s3_base + "/" + scope + "/input.pbf";
    shell_exec("aws s3 cp " + s3_input + " " + pbf_path);
  }

  const std::string scope_prefix  = s3_scope_prefix(s3_base, scope);
  const std::string version_prefix = scope_prefix + "/" + version;

  // Build
  const std::string build_dir = "/app/valhalla/build";
  shell_exec("rm -rf " + tile_dir + " && mkdir -p " + tile_dir);
  shell_exec("valhalla_build_admins -c " + cfg + " " + pbf_path);
  shell_exec("valhalla_build_tiles -c " + cfg + " " + pbf_path);
  shell_exec("valhalla_build_extract -c " + cfg + " -v -t");

  // Archive: include build artifacts + live tz_world.sqlite (shared read-only input)
  const std::string archive = build_dir + "/routing-data.tar";
  shell_exec("tar -cf " + archive +
             " -C " + build_dir + " tiles tiles.tar traffic.tar" +
             " -C /app/valhalla tz_world.sqlite");
  shell_exec("aws s3 cp " + archive + " " + version_prefix + "/routing-data.tar");

  // Update latest pointer
  write_s3_latest(scope_prefix, version);

  // Cleanup
  shell_exec("rm -rf " + build_dir);
  shell_exec("rm -f " + pbf_path);

  return json_ok("build complete: " + version_prefix);
}

std::string do_terminate() {
#ifdef ENABLE_SERVICES
  prime_server::quiesce(1, 1);
#endif
  return json_ok("terminating");
}

std::string do_restart(const boost::property_tree::ptree& config) {
  const std::string s3_base = config.get<std::string>("admin.s3_downloads_base", "");
  if (s3_base.empty())
    return json_error("admin.s3_downloads_base not configured");

  const std::string scope_prefix = s3_base + "/global";
  const std::string version      = read_s3_latest(scope_prefix);
  if (version.empty())
    return json_error("No latest global build found at " + scope_prefix + "/latest");

  const std::string tile_dir = config.get<std::string>("mjolnir.tile_dir");
  const std::string archive  = "/app/valhalla/tiles-current.tar";

  shell_exec("aws s3 cp " + scope_prefix + "/" + version +
             "/routing-data.tar " + archive);
  shell_exec("rm -rf " + tile_dir + " && mkdir -p " + tile_dir);
  shell_exec("tar -xf " + archive + " -C /app/valhalla");
  shell_exec("rm -f " + archive);

#ifdef ENABLE_SERVICES
  prime_server::quiesce(1, 1);
#endif
  return json_ok("reloaded tiles " + version + ", restarting");
}

} // namespace

namespace valhalla {
namespace loki {

std::string loki_worker_t::admin(Api& request) {
  try {
    std::string sub_action;
    const auto& args = request.options().admin_args();
    auto it = args.find("action");
    if (it != args.end())
      sub_action = it->second;

    std::string org_id_arg;
    auto org_it = args.find("org_id");
    if (org_it != args.end())
      org_id_arg = org_it->second;

    // Optional explicit version tag; auto-generated if absent
    std::string version_arg;
    auto ver_it = args.find("version");
    if (ver_it != args.end())
      version_arg = ver_it->second;

    LOG_INFO("Admin action: " + sub_action);

    if (sub_action == "build" || sub_action == "build_global") {
      const std::string build_type = (sub_action == "build_global" || org_id_arg.empty())
          ? "global" : "org";
      const std::string version = version_arg.empty() ? make_version(config) : version_arg;
      ActiveBuild::instance().start(build_type, org_id_arg);
      std::thread([this, oid = org_id_arg, ver = version]() {
        try {
          do_build(oid, ver, config);
        } catch (const std::exception& e) {
          LOG_ERROR("Admin build failed: " + std::string(e.what()));
        }
        ActiveBuild::instance().clear();
      }).detach();
      return json_ok("build started: " + version);
    } else if (sub_action == "terminate") {
      return do_terminate();
    } else if (sub_action == "restart") {
      return do_restart(config);
    } else {
      return json_error("unknown action: " + sub_action);
    }
  } catch (const std::exception& e) {
    return json_error(std::string(e.what()));
  }
}

} // namespace loki
} // namespace valhalla
