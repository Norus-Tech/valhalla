#include <valhalla/sif/costfilter.h>

#include <sstream>
#include <string>
#include <utility>

namespace valhalla {
namespace sif {

// CostFilterNode::CostFilterNode() : handler_(Filters::True) {}

CostFilterNode::CostFilterNode(const Costing_Filter& filter)
  : tag_(filter.tag()),
    literal_(filter.has_data()? filter.data(): ""),
    handler_(get_filter_handler(filter)),
    args_(filter.children().begin(), filter.children().end()) {}

CostFilterNode::CostFilterNode (const FilterFactory& factory) : handler_(factory) {}

bool CostFilterNode::operator() (const Costing_Options& options,
                                 CostFilterArg& result,
                                 const DirectedEdge* edge,
                                 const CustomAttributes& attrs) const {
  const auto f = handler_(options, this);
  f(result, edge, args_, attrs);
  LOG_DEBUG(to_string() + ": " + (result.value.empty() ? "()" : result.value));
  return result.value != "";
}

bool CostFilterNode::operator() (const Costing_Options& options,
                                 const DirectedEdge* edge,
                                 const CustomAttributes& attrs) const {
  CostFilterArg result;
  const auto f = handler_(options, this);
  f(result, edge, args_, attrs);
  LOG_DEBUG(to_string() + ": " + (result.value.empty() ? "()" : result.value));
  return result.value != "";
}

std::string CostFilterNode::to_string() const {
  return to_string(0);
}

std::string CostFilterNode::to_string(int indent) const {
  std::stringstream ss;
  ss << "{" << tag_;
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

FilterFactory get_filter_handler(const Costing_Filter& filter) {
  static const FilterFactory True = [](const Costing_Options& /*options*/, const CostFilterNode* /*self*/)  {
    return [](CostFilterArg& result,
              const DirectedEdge*  /*edge*/,
              const std::vector<CostFilterNode>&  /*args*/,
              const CustomAttributes& /*attrs*/) {
      result.value = "1";
    };
  };

  static const FilterFactory False = [](const Costing_Options& /*options*/, const CostFilterNode* /*self*/) {
    return [](CostFilterArg& result,
              const DirectedEdge*  /*edge*/,
              const std::vector<CostFilterNode>&  /*args*/,
              const CustomAttributes& /*attrs*/) {
      result.value = "";
    };
  };

  static const FilterFactory Literal = [](const Costing_Options& /*options*/, const CostFilterNode* self) {
    return [self](CostFilterArg& result,
                   const DirectedEdge* /*edge*/,
                   const std::vector<CostFilterNode>& /*args*/,
                   const CustomAttributes& /*attrs*/) {
      result.value = self->literal();
      // result.value = "";
    };
  };

  static const FilterFactory Or = [](const Costing_Options& options, const CostFilterNode*  /*self*/) {
    return [&options](CostFilterArg& result,
                      const DirectedEdge* edge,
                      const std::vector<CostFilterNode>& args,
                      const CustomAttributes& attrs) {
      for (const auto &arg: args) {
        arg(options, result, edge, attrs);
        if (!result.value.empty()) return;
      }
    };
  };

  static const FilterFactory And = [](const Costing_Options& options, const CostFilterNode*  /*self*/) {
    return [&options](CostFilterArg& result,
                      const DirectedEdge* edge,
                      const std::vector<CostFilterNode>& args,
                      const CustomAttributes& attrs) {
      for (const auto &arg: args) {
        arg(options, result, edge, attrs);
        if (result.value.empty()) return;
      }
    };
  };

  static const FilterFactory Not = [](const Costing_Options& options, const CostFilterNode*  /*self*/) {
    return [&options](CostFilterArg& result,
                      const DirectedEdge* edge,
                      const std::vector<CostFilterNode>& args,
                      const CustomAttributes& attrs) {
      if (!args.size()) {
        // TODO:error or set result "null"
        result.value = "";
        return;
      }
      args[0](options, result, edge, attrs);
      result.value = result.value.empty () ? "1" : "";
    };
  };

  static const FilterFactory Eq = [](const Costing_Options& options, const CostFilterNode*  /*self*/) {
    return [&options](CostFilterArg& result,
                      const DirectedEdge* edge,
                      const std::vector<CostFilterNode>& args,
                      const CustomAttributes& attrs) {
      if (!args.size()) return; // TODO:error or set result "null"
      args[0](options, result, edge, attrs);
      const auto value = result.value;
      for (size_t i=1; i < args.size(); ++i) {
        args[i](options, result, edge, attrs);
        if (result.value != value) {
          result.value = "";
          return;
        }
      }
    };
  };

  static const FilterFactory Request = [](const Costing_Options& options, const CostFilterNode*  /*self*/) {
    return [&options](CostFilterArg& result,
                      const DirectedEdge* edge,
                      const std::vector<CostFilterNode>& args,
                      const CustomAttributes& attrs) {
      if (!args.size()) {
        // TODO:error or set result "null"
        result.value = "";
        return;
      }
      // get key
      args[0](options, result, edge, attrs);
      if (result.value.empty()) {
        // TODO:error?
        return;
      }
      for (const auto &kv: options.filter_args()) {
        if (kv.first == result.value) {
          result.value = kv.second;
          return;
        }
      }
      // TODO:error?
      result.value = "";
    };
  };

  static const FilterFactory Get = [](const Costing_Options&  options, const CostFilterNode*  /*self*/) {
    return [&options](CostFilterArg& result,
                      const DirectedEdge* edge,
                      const std::vector<CostFilterNode>& args,
                      const CustomAttributes& attrs) {
      if (!args.size()) {
        // TODO:error or set result "null"
        result.value = "";
        return;
      }
      args[0](options, result, edge, attrs);
      if (result.value.empty()) {
        // TODO:error?
        return;
      }
      auto value = attrs.find(result.value);
      result.value = (value == attrs.cend() ? "" : value->second);
    };
  };

  // Contains: args[0] = comma-delimited haystack, args[1] = needle.
  // Returns needle if found, "" if not
  static const FilterFactory Contains = [](const Costing_Options& options, const CostFilterNode* /*self*/) {
       return [&options](CostFilterArg& result,
                      const DirectedEdge* edge,
                      const std::vector<CostFilterNode>& args,
                      const CustomAttributes& attrs) {
      if (args.size() < 2) {
        result.value = "";
        return;
      }
      // evaluate haystack
      args[0](options, result, edge, attrs);
      const auto haystack = result.value;
      if (haystack.empty()) {
        result.value = "";
        return;
      }
      // evaluate needle
      args[1](options, result, edge, attrs);
      const auto needle = result.value;
      if (needle.empty()) {
        result.value = "";
        return;
      }
      // split haystack on ',' and check for exact token match
      std::istringstream ss(haystack);
      std::string token;
      while (std::getline(ss, token, ',')) {
        if (token == needle) {
          result.value = needle;
          return;
        }
      }
      result.value = "";
    };
  };

  static const std::unordered_map<std::string, FilterFactory> filter_handlers = {
    {"true",     True},
    {"false",    False},
    {"literal",  Literal},
    {"or",       Or},
    {"and",      And},
    {"not",      Not},
    {"!",        Not},
    {"==",       Eq},
    {"eq",       Eq},
    {"request",  Request},
    {"get",      Get},
    {"contains", Contains},
  };

  if (filter.has_data()) {    // node, tag shouldn't matter
    return Literal;
  }
  const auto& tag = filter.tag();
  const auto& handler = filter_handlers.find(tag);
  if (handler == filter_handlers.cend()) {
    LOG_WARN("No filter handler for '" + tag + "'");
    // TODO: throw
    return True;
  }
  return handler->second;
}

} // namespace sif

} // namespace valhalla
