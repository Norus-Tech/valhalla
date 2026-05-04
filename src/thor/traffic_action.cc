#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <numeric>
#include <microtar.h>
#include "thor/worker.h"
#include <valhalla/baldr/attributes_controller.h>
#include "baldr/predictedspeeds.h"
#include "baldr/traffictile.h"
#include "mjolnir/graphtilebuilder.h"
#include "thor/triplegbuilder.h"
#include "midgard/pointll.h"

using namespace valhalla::baldr;
using namespace valhalla::mjolnir;
using namespace valhalla::midgard;

// Mmap and mtar structs copied from test/test.cc.
struct MMap {
  MMap(const char* filename) {
    fd = open(filename, O_RDWR);
    struct stat s;

#ifdef _MSC_VER
#define fstat(fd, s) _fstat64(fd, s)
#endif
    fstat(fd, &s);

    data = mmap(0, s.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    length = s.st_size;
  }

  ~MMap() {
    munmap(data, length);
    close(fd);
  }

  int fd;
  void* data;
  size_t length;
};

class MMapGraphMemory final : public GraphMemory {
public:
  MMapGraphMemory(std::shared_ptr<MMap> mmap, char* data_, size_t size_) : mmap_(std::move(mmap)) {
    data = data_;
    size = size_;
  }

private:
  const std::shared_ptr<MMap> mmap_;
};

/** Copy of raw header for use with sizeof() **/
typedef struct {
  char name[100];
  char mode[8];
  char owner[8];
  char group[8];
  char size[12];
  char mtime[12];
  char checksum[8];
  char type;
  char linkname[100];
  char _padding[255];
} mtar_raw_header_t_;

struct TrafficSpeeds {
  uint8_t constrained_flow_speed;
  uint8_t free_flow_speed;
  std::optional<std::array<int16_t, kCoefficientCount>> coefficients;
};

void update_tile(const std::string& tile_dir,
                 const GraphId& tile_id,
                 const std::unordered_map<uint32_t, TrafficSpeeds>& speeds) {
  auto tile_path = tile_dir + std::filesystem::path::preferred_separator + GraphTile::FileSuffix(tile_id);
  if (!std::filesystem::exists(tile_path)) {
    LOG_ERROR("No tile at " + tile_path);
    return;
  }

  GraphTileBuilder tile_builder(tile_dir, tile_id, false);

  size_t pred_count = 0;
  for (uint32_t j = 0; j < tile_builder.header()->directededgecount(); ++j)
    pred_count += speeds.count(j) && speeds.at(j).coefficients.has_value();

  std::vector<DirectedEdge> directededges;
  directededges.reserve(tile_builder.header()->directededgecount());
  for (uint32_t j = 0; j < tile_builder.header()->directededgecount(); ++j) {
    DirectedEdge& de = tile_builder.directededge(j);
    auto it = speeds.find(j);
    if (it != speeds.end()) {
      const auto& ts = it->second;
      if (ts.constrained_flow_speed)
        de.set_constrained_flow_speed(ts.constrained_flow_speed);
      if (ts.free_flow_speed)
        de.set_free_flow_speed(ts.free_flow_speed);
      if (ts.coefficients) {
        tile_builder.AddPredictedSpeed(j, *ts.coefficients, pred_count);
        de.set_has_predicted_speed(true);
      }
    }
    directededges.emplace_back(std::move(de));
  }
  tile_builder.UpdatePredictedSpeeds(directededges);
}

namespace valhalla {
namespace thor {

template <typename Predicate> inline void remove_path_edges(valhalla::Location& loc, Predicate pred) {
  auto new_end = std::remove_if(loc.mutable_correlation()->mutable_edges()->begin(),
                                loc.mutable_correlation()->mutable_edges()->end(), pred);
  int start_idx = std::distance(loc.mutable_correlation()->mutable_edges()->begin(), new_end);
  loc.mutable_correlation()->mutable_edges()->DeleteSubrange(start_idx,
                                                             loc.correlation().edges_size() -
                                                                 start_idx);

  new_end = std::remove_if(loc.mutable_correlation()->mutable_filtered_edges()->begin(),
                           loc.mutable_correlation()->mutable_filtered_edges()->end(), pred);
  start_idx = std::distance(loc.mutable_correlation()->mutable_filtered_edges()->begin(), new_end);
  loc.mutable_correlation()
      ->mutable_filtered_edges()
      ->DeleteSubrange(start_idx, loc.correlation().filtered_edges_size() - start_idx);
}

inline bool is_through_point(const valhalla::Location& l) {
  return l.type() == valhalla::Location::kThrough || l.type() == valhalla::Location::kBreakThrough;
}

inline bool is_break_point(const valhalla::Location& l) {
  return l.type() == valhalla::Location::kBreak || l.type() == valhalla::Location::kBreakThrough;
}

inline bool is_highly_reachable(const valhalla::Location& loc, const valhalla::PathEdge& edge) {
  return static_cast<google::protobuf::uint32>(edge.inbound_reach()) >= loc.minimum_reachability() &&
         static_cast<google::protobuf::uint32>(edge.outbound_reach()) >= loc.minimum_reachability();
}

/**
 * Check if the paths meet at opposing edges (but not at a node). If so, add an intermediate location
 * so that the shape / distance along the path is adjusted at the location.
 */
bool intermediate_loc_edge_trimming(
    valhalla::Location& loc,
    const GraphId& in,
    const GraphId& out,
    std::unordered_map<size_t, std::pair<EdgeTrimmingInfo, EdgeTrimmingInfo>>& edge_trimming,
    const size_t path_index,
    const bool arrive_by) {
  // Find the path edges within the locations.
  auto in_pe = std::find_if(loc.correlation().edges().begin(), loc.correlation().edges().end(),
                            [&in](const valhalla::PathEdge& e) { return e.graph_id() == in; });
  auto out_pe = std::find_if(loc.correlation().edges().begin(), loc.correlation().edges().end(),
                             [&out](const valhalla::PathEdge& e) { return e.graph_id() == out; });

  // Could not find the edges. This seems like it should not happen. Log a warning
  // and do not insert an intermediate location.
  if (in_pe == loc.correlation().edges().end() || out_pe == loc.correlation().edges().end()) {
    LOG_WARN("Could not find connecting edges within the intermediate loc edge trimming");
    return false;
  }

  // Just set the edge index on the location for now then in triplegbuilder set to the shape index
  loc.mutable_correlation()->set_leg_shape_index(path_index + (arrive_by ? 1 : 0));
  // If the intermediate point is at a node we dont need to trim the edge we just set the edge index
  if (in_pe->begin_node() || in_pe->end_node() || out_pe->begin_node() || out_pe->end_node()) {
    // In this case we won't add a duplicate edge so we cant increment for arrive by
    loc.mutable_correlation()->set_leg_shape_index(path_index);
    return true;
  }

  // So what we are doing below is we are telling tripleg builder how to trim the two edges that come
  // together at an intermediate location. There are two cases, one where the route keeps going on the
  // same edge and one where the route does a uturn onto to opposing edge. In both cases we get two
  // edges in the final route just for simplicities sake. We could technically only do two when its
  // the uturn case. The way we do this is we specify a way to trim the shape of each edge. We use the
  // lat lon of the intermediate location to fix the geometry and we use the distance along to tell
  // trip leg builder how to cut the shape. We have to have at least one cut on each edge on either
  // side of the intermediate location. If we have multiple intermediate locations on a single edge we
  // will need to cut the edge based on the previous intermediate location we processed.

  PointLL snap_ll(in_pe->ll().lng(), in_pe->ll().lat());
  double dist_along = in_pe->percent_along();

  // Cut the first edges end off back to where the location lands along it
  auto inserted = edge_trimming.insert(
      {path_index + (arrive_by ? 1 : 0), {{false, PointLL(), 0.0}, {true, snap_ll, dist_along}}});
  // If it was already there we need to update it, should only happen for depart at (left to right)
  if (!inserted.second) {
    inserted.first->second.second = EdgeTrimmingInfo{true, snap_ll, dist_along};
  }

  // We need to use the distance along from the edge exiting the intermediate location to handle the
  // case when its a uturn at a via using the opposing edge that came into the via. Basically we need
  // to invert the distance along because we inverted the direction we are traveling along the edge.
  // This is handled automatically by using the correct exiting edge
  dist_along = out_pe->percent_along();

  // Cut the second edges beginning off up to where the location lands along it
  inserted = edge_trimming.insert(
      {path_index + (arrive_by ? 0 : 1), {{true, snap_ll, dist_along}, {false, PointLL(), 1.0}}});
  // If it was already there we need to update it, should only happen for arrive by (right to left)
  if (!inserted.second) {
    inserted.first->second.first = EdgeTrimmingInfo{true, snap_ll, dist_along};
  }

  return false;
}

  void thor_worker_t::traffic(Api& request) {
  // time this whole method and save that statistic
  auto _ = measure_scope_time(request);

  const Options& options = request.options();
  controller = AttributesController(options);
  auto costing = parse_costing(request);

  // Things we'll need
  // TripRoute* route = nullptr;
  GraphId last_edge;
  std::unordered_map<size_t, std::pair<EdgeTrimmingInfo, EdgeTrimmingInfo>> edge_trimming;
  std::vector<thor::PathInfo> path;
  std::vector<std::string> algorithms;
  valhalla::Trip& trip = *request.mutable_trip();
  trip.mutable_routes()->Reserve(options.alternates() + 1);

  graph_tile_ptr tile = nullptr;
  auto route_two_locations = [&, this](auto& origin, auto& destination) -> bool {
    // Get the algorithm type for this location pair
    thor::PathAlgorithm* path_algorithm =
        this->get_path_algorithm(costing, *origin, *destination, options);
    path_algorithm->Clear();
    algorithms.push_back(path_algorithm->name());
    LOG_INFO(std::string("algorithm::") + path_algorithm->name());

    // If we are continuing through a location we need to make sure we
    // only allow the edge that was used previously (avoid u-turns)
    if (is_through_point(*origin) && last_edge.Is_Valid()) {
      remove_path_edges(*origin,
                        [&last_edge](const auto& edge) { return edge.graph_id() != last_edge; });
    }
    // Get best path and keep it
    auto temp_paths = this->get_path(path_algorithm, *origin, *destination, costing, options);
    if (temp_paths.empty())
      return false;

    for (auto& temp_path : temp_paths) {
      last_edge = temp_path.back().edgeid;

      // Merge through legs by updating the time and splicing the lists
      if (!path.empty()) {
        // When stitching routes at an intermediate location we need to store information about where
        // along the edge it happened so triplegbuilder can properly cut the shape where the location
        // was and store that info to be serialized int he output
        auto at_node =
            intermediate_loc_edge_trimming(*origin, path.back().edgeid, temp_path.front().edgeid,
                                           edge_trimming, path.size() - 1, false);

        // Connects via the same edge so we only need it once
        if (path.back().edgeid == temp_path.front().edgeid && at_node) {
          path.pop_back();
        }

        path.insert(path.end(), temp_path.begin(), temp_path.end());
      } // Didnt need to merge
      else {
        path.swap(temp_path);
      }

      // // Build trip path for this leg and add to the result if this
      // // location is a BREAK or if this is the last location
      // if (is_break_point(*destination)) {
      //   // Move origin back to the last break
      //   while (!is_break_point(*origin)) {
      //     --origin;
      //   }

      //   // Form output information based on path edges.
      //   if (trip.routes_size() == 0 || options.alternates() > 0) {
      //     route = trip.mutable_routes()->Add();
      //     route->mutable_legs()->Reserve(options.locations_size());
      //   }
      //   auto& leg = *route->mutable_legs()->Add();
      //   thor::TripLegBuilder::Build(options, controller, *reader, mode_costing, path.begin(),
      //                               path.end(), *origin, *destination, leg, algorithms, interrupt,
      //                               edge_trimming, {std::next(origin), destination});

      //   path.clear();
      //   edge_trimming.clear();
      // }
    }

    // if we just made a leg that means we are done recording which algorithms were used
    // if (path.empty())
    //   algorithms.clear();

    return true;
  };

  auto correlated = options.locations();

  // For each pair of locations
  auto destination = ++correlated.begin();
  while (destination != correlated.end()) {
    auto origin = std::prev(destination);
    if (!route_two_locations(origin, destination)) {
      // no retry
      throw valhalla_exception_t{442};
    }
    ++destination;
  }
  // by this time we should have path;
  // for (auto& item: path) {
  //   std::cerr << "Pathi item " << item << "\n";
  // }

  std::unordered_set<GraphId> regular_edges;
  for (const auto& location : options.locations()) {
    for (const auto& edge : location.correlation ().edges()) {
      regular_edges.insert(GraphId(edge.graph_id()));
    }
  }


  // build full set of edges to update including shortcuts
  // map of graphid -> speed for all edges (regular + shortcuts)
  std::unordered_map<GraphId, uint32_t> edges_to_update;
  std::unordered_map<GraphId, std::vector<uint32_t>> shortcut_speeds;
  for (const auto& edge : path) {
    edges_to_update[edge.edgeid] = options.path_speed_live();
  }
  for (const auto& edge : path) {
    auto shortcut_id = reader->GetShortcut(edge.edgeid);
    if (!shortcut_id.Is_Valid())
      continue;
    auto constituents = reader->RecoverShortcut(shortcut_id);
    uint32_t speed_sum = 0, count = 0;
    for (const auto& constituent : constituents) {
      if (edges_to_update.count(constituent)) {
        speed_sum += options.path_speed_live();
        ++count;
      }
    }
    if (count > 0)
      shortcut_speeds[shortcut_id].push_back(speed_sum / count);
  }
  for (const auto& [shortcut_id, speeds] : shortcut_speeds) {
    uint32_t avg = std::accumulate(speeds.begin(), speeds.end(), 0u) / speeds.size();
    edges_to_update[shortcut_id] = avg;
  }

  // write live speeds into mmap'd traffic tar
  if (options.path_speed_live() > 0) {
    // const auto traffic_extract = config.get<std::string>("mjolnir.traffic_extract", "");
    if (traffic_extract.empty())
      throw valhalla_exception_t{199};


    const auto memory = std::make_shared<MMap>(traffic_extract.c_str());
    mtar_t tar;
    tar.pos = 0;
    tar.stream = memory->data;
    tar.read = [](mtar_t* tar, void* data, unsigned size) -> int {
      memcpy(data, reinterpret_cast<char*>(tar->stream) + tar->pos, size);
      return MTAR_ESUCCESS;
    };
    tar.write = [](mtar_t* tar, const void* data, unsigned size) -> int {
      memcpy(reinterpret_cast<char*>(tar->stream) + tar->pos, data, size);
      return MTAR_ESUCCESS;
    };
    tar.seek = [](mtar_t* /*tar*/, unsigned /*pos*/) -> int { return MTAR_ESUCCESS; };
    tar.close = [](mtar_t* /*tar*/) -> int { return MTAR_ESUCCESS; };

    mtar_header_t tar_header;
    while (mtar_read_header(&tar, &tar_header) != MTAR_ENULLRECORD) {
      TrafficTile tile(
          std::make_unique<MMapGraphMemory>(memory,
                                           reinterpret_cast<char*>(tar.stream) + tar.pos +
                                               sizeof(mtar_raw_header_t_),
                                           tar_header.size));
      GraphId tile_id(tile.header->tile_id);

      for (uint32_t index = 0; index < tile.header->directed_edge_count; ++index) {
        GraphId edge_id(tile_id.tileid(), tile_id.level(), index);
        auto it = edges_to_update.find(edge_id);
        if (it == edges_to_update.end())
          continue;
        uint32_t speed_kph = it->second;
        auto* current = const_cast<TrafficSpeed*>(tile.speeds + index);
        if (speed_kph == kUnlimitedSpeedLimit) {
          // clear the live speed
          // std::cerr << "Clearing speed for " << edge_id << "\n";
          current->overall_encoded_speed = 0;
          current->encoded_speed1 = 0;
          current->breakpoint1 = 0;
        } else {
          // std::cerr << "Setting speed for " << edge_id << " to " << speed_kph << "\n";
          current->overall_encoded_speed = speed_kph >> 1;
          current->encoded_speed1 = speed_kph >> 1;
          current->breakpoint1 = 255;
        }
      }
      mtar_next(&tar);
    }
  }

  // write constrained/freeflow/predicted via GraphTileBuilder, grouped by tile
  bool write_constrained = options.path_speed_constrained() > 0;
  bool write_freeflow = options.path_speed_freeflow() > 0;
  bool write_predicted = options.path_speed_predicted_size() == kBucketsPerWeek;

  if (write_constrained || write_freeflow || write_predicted) {
    // Compress predicted speeds once — same for all edges
    std::optional<std::array<int16_t, kCoefficientCount>> coefficients;
    if (write_predicted) {
      std::vector<float> buckets(kBucketsPerWeek);
      for (int i = 0; i < static_cast<int>(kBucketsPerWeek); ++i)
        buckets[i] = static_cast<float>(options.path_speed_predicted(i));
      coefficients = baldr::compress_speed_buckets(buckets.data());
    }

    // group edges by tile
    std::unordered_map<GraphId, std::unordered_map<uint32_t, TrafficSpeeds>> tile_speeds;
    for (const auto& [edge_id, speed] : edges_to_update) {
      auto tile_base = edge_id.Tile_Base();
      auto& ts = tile_speeds[tile_base][edge_id.id()];
      if (write_constrained)
        ts.constrained_flow_speed = options.path_speed_constrained();
      if (write_freeflow)
        ts.free_flow_speed = options.path_speed_freeflow();
      if (write_predicted)
        ts.coefficients = coefficients;
    }

    // const auto tile_dir = config.get<std::string>("mjolnir.tile_dir");
    for (auto& [tile_id, speeds] : tile_speeds) {
      // open tile builder and update directed edges directly
      GraphTileBuilder tile_builder(tile_dir, tile_id, false);
      std::vector<DirectedEdge> directededges;
      directededges.reserve(tile_builder.header()->directededgecount());
      size_t pred_count = 0;
      for (uint32_t j = 0; j < tile_builder.header()->directededgecount(); ++j)
        pred_count += speeds.count(j) && write_predicted;

      for (uint32_t j = 0; j < tile_builder.header()->directededgecount(); ++j) {
        DirectedEdge& de = tile_builder.directededge(j);
        auto it = speeds.find(j);
        if (it != speeds.end()) {
          const auto& ts = it->second;
          if (write_constrained)
            de.set_constrained_flow_speed(ts.constrained_flow_speed);
          if (write_freeflow)
            de.set_free_flow_speed(ts.free_flow_speed);
          if (write_predicted && ts.coefficients) {
            tile_builder.AddPredictedSpeed(j, *ts.coefficients, pred_count);
            de.set_has_predicted_speed(true);
          }
        }
        directededges.emplace_back(std::move(de));
      }
      tile_builder.UpdatePredictedSpeeds(directededges);
    }
    for (auto& [tile_id, speeds] : tile_speeds) {
      update_tile(tile_dir, tile_id, speeds);
    }
  }



  // assign changed locations
  // *request.mutable_options()->mutable_locations() = std::move(correlated);
}
} // namespace thor
} // namespace valhalla
