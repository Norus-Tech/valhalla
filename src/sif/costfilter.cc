#include <valhalla/sif/costfilter.h>

#include <string>
#include <utility>

namespace valhalla {
namespace sif {

// CostFilterNode::CostFilterNode() : handler_(Filters::True) {}

CostFilterNode::CostFilterNode(const Costing_Filter& filter)
  : tag_(filter.tag()),
    handler_(get_filter_handler(filter)),
    args_(filter.children().begin(), filter.children().end()) {
  if (filter.has_data()) {    // node, tag shouldn't matter
    literal_ = filter.data();
    return;
  }
  // const auto& tag = filter.tag();
  // const auto& handler = filter_handlers.find(tag);
  // if (handler == filter_handlers.end()) {
  //   LOG_WARN("No filter handler for '" + tag + "'");
  //   // TODO: throw
  //   return;
  // }
  // for (const auto& child: filter.children()) {
  //   // CostFilterNode arg(child);
  //   args_.emplace_back(child);
  // }
}

CostFilterNode::CostFilterNode (const FilterFactory& factory) : handler_(factory) {}

bool CostFilterNode::operator() (const Costing_Options& options,
                                 CostFilterArg& result,
                                 const DirectedEdge* edge,
                                 const CustomAttributes& attrs) const {
  const auto f = handler_(options, this);
  f(result, edge, args_, attrs);
  return result.value != "";
}

bool CostFilterNode::operator() (const Costing_Options& options,
                                 const DirectedEdge* edge,
                                 const CustomAttributes& attrs) const {
  CostFilterArg result;
  const auto f = handler_(options, this);
  f(result, edge, args_, attrs);
  return result.value != "";
}

std::string CostFilterNode::to_string() const {
  return to_string(0);
}

std::string CostFilterNode::to_string(int indent) const {
  std::stringstream ss;
  ss << "{" << tag_;
  // auto name = std::find_if(sif::filter_handlers.begin(), sif::filter_handlers.end(),
  //                          [this](FilterFactory& f) { return &f == &handler_; });
  // if (name != filter_handlers.cend ()) {
  //   ss << name->first;
  // }
  if (tag_ == "literal") {
    ss << " " << literal_;
  } else {
    for (const auto &arg : args_) {
      if (arg.tag_ == "literal") {
        ss << " \"" << arg.literal_ << "\"";
        continue;
      }
      ss << "\n";
      for (int i=-1; i<indent; i++) {
        ss << "  ";
      }
      ss << arg.to_string(indent+1);
    }
  }
  ss << "}";
  return ss.str();
}

CostFilter::CostFilter() : root_(Filters::True) {}

FilterFactory get_filter_handler(const Costing_Filter& filter) {

  if (filter.has_data()) {    // node, tag shouldn't matter
    return Filters::Literal;
  }
  const auto& tag = filter.tag();
  LOG_INFO("tag: " + tag);
  const auto& handler = filter_handlers.find(tag);
  if (handler == filter_handlers.end()) {
    LOG_WARN("No filter handler for '" + tag + "'");
    // TODO: throw
    return Filters::True;
  }
  return handler->second;;
}

} // namespace sif

} // namespace valhalla
