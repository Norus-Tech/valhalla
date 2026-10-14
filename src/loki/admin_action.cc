#include "loki/worker.h"
#include "baldr/graphtileheader.h"
#include "loki/build_state.h"
#include "midgard/logging.h"
#include "config.h"

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

  const std::string s3_pbf = config.get<std::string>("admin.s3_pbf_url", "");
  if (s3_pbf.empty())
    throw std::runtime_error("admin.s3_pbf_url not configured");

  const std::string base_pbf = "/app/valhalla/base.osm.pbf";
  const std::string osc_path = "/app/valhalla/build.osc";

  // Pull base PBF (download once; may already be present from a prior build)
  shell_exec("aws s3 cp " + s3_pbf + " " + base_pbf);

  if (org_id.empty()) {
    // Global build: apply global OSC onto base PBF
    scope    = "global";
    pbf_path = "/app/valhalla/build-global.osm.pbf";
    const std::string s3_osc = s3_base + "/global/" + version + "/changes.osc";
    shell_exec("aws s3 cp " + s3_osc + " " + osc_path);
    shell_exec("osmium apply-changes " + base_pbf + " " + osc_path +
               " -o " + pbf_path + " --overwrite");
  } else {
    // Org build: apply org OSC onto base PBF
    scope    = "org/" + org_id;
    pbf_path = "/app/valhalla/build-" + org_id + ".osm.pbf";
    const std::string s3_osc = s3_base + "/org/" + org_id + "/" + version + "/changes.osc";
    shell_exec("aws s3 cp " + s3_osc + " " + osc_path);
    shell_exec("osmium apply-changes " + base_pbf + " " + osc_path +
               " -o " + pbf_path + " --overwrite");
  }

  const std::string scope_prefix  = s3_scope_prefix(s3_base, scope);
  const std::string version_prefix = scope_prefix + "/" + version;

  // Build
  const std::string build_dir = "/app/valhalla/build";
  shell_exec("rm -rf " + tile_dir + " && mkdir -p " + tile_dir);
  shell_exec("valhalla_build_admins -c " + cfg + " " + pbf_path);
  shell_exec("valhalla_build_tiles -c " + cfg + " " + pbf_path);
  shell_exec("valhalla_build_extract -c " + cfg + " -v -t");

  // Upload tiles.tar and traffic.tar directly -- no outer archive needed
  const std::string tiles_tar   = build_dir + "/tiles.tar";
  const std::string traffic_tar = build_dir + "/traffic.tar";

  auto upload_with_checksum = [&](const std::string& local,
                                  const std::string& s3_dest) {
    shell_exec("aws s3 cp " + local + " " + s3_dest);
    const std::string sha = shell_exec(
        "sha256sum " + local + " | awk '{print $1}'");
    const std::string sha_file = local + ".sha256";
    FILE* f = fopen(sha_file.c_str(), "w");
    if (f) { fputs(sha.c_str(), f); fclose(f); }
    shell_exec("aws s3 cp " + sha_file + " " + s3_dest + ".sha256");
  };

  upload_with_checksum(tiles_tar,   version_prefix + "/tiles.tar");
  upload_with_checksum(traffic_tar, version_prefix + "/traffic.tar");

  // For global builds: upload content-addressed base files
  if (org_id.empty()) {
    auto upload_base_file = [&](const std::string& local_path,
                                const std::string& name,
                                const std::string& ext) {
      // Compute sha256
      const std::string sha = shell_exec(
          "sha256sum " + local_path + " | awk '{print $1}'");
      const std::string sha_trimmed = sha.substr(0, sha.find_first_of(" \t\n\r"));
      const std::string s3_key = s3_base + "/downloads/base/" + name + "/"
                                 + sha_trimmed + "." + ext;
      const std::string latest_key = s3_base + "/downloads/base/" + name + "/latest";

      // Upload file and sidecar
      shell_exec("aws s3 cp " + local_path + " " + s3_key);
      const std::string sha_file = "/tmp/valhalla-" + name + ".sha256";
      FILE* f = fopen(sha_file.c_str(), "w");
      if (f) { fputs(sha_trimmed.c_str(), f); fclose(f); }
      shell_exec("aws s3 cp " + sha_file + " " + s3_key + ".sha256");

      // Update latest pointer
      const std::string latest_tmp = "/tmp/valhalla-" + name + "-latest.txt";
      FILE* lf = fopen(latest_tmp.c_str(), "w");
      if (lf) { fputs(s3_key.c_str(), lf); fclose(lf); }
      shell_exec("aws s3 cp " + latest_tmp + " " + latest_key);
    };

    upload_base_file(tile_dir + "/admins.sqlite", "admins", "sqlite");
    upload_base_file("/app/valhalla/tz_world.sqlite", "tz_world", "sqlite");
  }

  // Update latest pointer and tile version
  write_s3_latest(scope_prefix, version);

  // Write routing tile version (kGraphTileVersion + "-yassuh") for manifest
  // Only written for global scope -- org builds inherit the global tile version
  if (org_id.empty()) {
    const std::string tile_ver = std::string(VALHALLA_PRINT_VERSION) + "-yassuh";
    const std::string tmp_ver = "/tmp/valhalla-tile-version.txt";
    FILE* f = fopen(tmp_ver.c_str(), "w");
    if (f) { fputs(tile_ver.c_str(), f); fclose(f); }
    shell_exec("aws s3 cp " + tmp_ver + " " + s3_base + "/global/tile_version");
  }

  // Cleanup
  shell_exec("rm -rf " + build_dir);
  shell_exec("rm -f " + pbf_path + " " + osc_path);

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

  const std::string scope_prefix   = s3_base + "/global";
  const std::string version        = read_s3_latest(scope_prefix);
  if (version.empty())
    return json_error("No latest global build found at " + scope_prefix + "/latest");

  const std::string valhalla_dir   = "/app/valhalla";
  const std::string version_file   = valhalla_dir + "/.current_version";
  const std::string ver_prefix     = scope_prefix + "/" + version;

  // Check if we already have this version -- skip download if so
  std::string current_version;
  FILE* vf = fopen(version_file.c_str(), "r");
  if (vf) {
    char buf[128] = {};
    if (fgets(buf, sizeof(buf), vf)) {
      current_version = buf;
      while (!current_version.empty() &&
             (current_version.back() == '\n' || current_version.back() == ' '))
        current_version.pop_back();
    }
    fclose(vf);
  }

  if (current_version == version &&
      !shell_exec("test -f " + valhalla_dir + "/tiles.tar && echo ok").empty() &&
      !shell_exec("test -f " + valhalla_dir + "/traffic.tar && echo ok").empty()) {
    LOG_INFO("Already at version " + version + ", skipping download.");
    return json_ok("already at version " + version + ", no restart needed");
  }

  LOG_INFO("Updating to version " + version + " (current: " +
           (current_version.empty() ? "none" : current_version) + ")");

  // Restore routing tiles -- valhalla reads directly from tiles.tar extract
  shell_exec("aws s3 cp " + ver_prefix + "/tiles.tar " + valhalla_dir + "/tiles.tar");

  // Restore traffic extract
  shell_exec("aws s3 cp " + ver_prefix + "/traffic.tar " + valhalla_dir + "/traffic.tar");

  // Restore base files -- skip if already present (they change rarely)
  auto restore_base = [&](const std::string& name, const std::string& ext) {
    const std::string dest       = valhalla_dir + "/" + name + "." + ext;
    const std::string latest_key = s3_base + "/base/" + name + "/latest";
    try {
      const std::string s3_key = shell_exec("aws s3 cp " + latest_key + " -");
      if (!s3_key.empty())
        shell_exec("aws s3 cp " + s3_key + " " + dest + ".tmp && mv " + dest + ".tmp " + dest);
    } catch (...) {
      LOG_WARN("Could not restore base file: " + name + "." + ext);
    }
  };

  restore_base("admins", "sqlite");
  // tz_world baked into tiles at build time, not needed at runtime

  // Write new version marker
  FILE* wf = fopen(version_file.c_str(), "w");
  if (wf) { fputs(version.c_str(), wf); fclose(wf); }

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
