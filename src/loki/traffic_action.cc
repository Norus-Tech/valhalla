#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "loki/worker.h"

using namespace valhalla::baldr;

namespace valhalla {
namespace loki {

void loki_worker_t::traffic(Api& request) {
  parse_costing(request);
}

} // namespace loki
} // namespace valhalla
