#ifndef VALHALLA_SIF_COSTFILTER_H_
#define VALHALLA_SIF_COSTFILTER_H_

#include <valhalla/valhalla.h>
#include <baldr/directededge.h>
#include <valhalla/proto/options.pb.h>
#include <proto_conversions.h>
#include <midgard/logging.h>

#include <boost/property_tree/ptree.hpp>
#include <unordered_map>
#include <utility>

namespace valhalla {
namespace sif {

using DirectedEdge = baldr::DirectedEdge;
const std::unordered_map<Costing::Type, std::string> kCostingNameMapping{
    {Costing::none_, "none"},
    {Costing::bicycle, "bicycle"},
    {Costing::bus, "bus"},
    {Costing::motor_scooter, "{Costing::motor_scooter}"},
    {Costing::multimodal, "multimodal"},
    {Costing::pedestrian, "pedestrian"},
    {Costing::transit, "transit"},
    {Costing::truck, "truck"},
    {Costing::motorcycle, "motorcycle"},
    {Costing::taxi, "taxi"},
    {Costing::auto_,"auto"},
    {Costing::bikeshare, "bikeshare"},
};

class CostFilter;
class CostFilterNode;
class CostFilterArg {
public:
//   void set_value (std::string value) {
//     value_ = std::move(value);
//   }
// protected:
//   int type_;
  std::string value;
};

using CustomAttributes = std::unordered_map<std::string,std::string>;
using FilterHandler = std::function<void(
                                         CostFilterArg&,
                                         const DirectedEdge*,
                                         const std::vector<CostFilterNode>&,
                                         const CustomAttributes&
                                         )>;
using FilterFactory = std::function<FilterHandler(const Costing_Options&, const CostFilterNode*)>;

class CostFilterNode {
public:
  // default filter just returns true (does nothing)
  CostFilterNode() = delete;

  CostFilterNode(const Costing_Filter& filter);

  CostFilterNode (const FilterFactory& factory);

  bool operator() (const Costing_Options& options,
                   CostFilterArg& result,
                   const DirectedEdge* edge,
                   const CustomAttributes& attrs) const;

  bool operator() (const Costing_Options& options,
                   const DirectedEdge* edge,
                   const CustomAttributes& attrs) const;

  std::string to_string() const;

  std::string to_string(int) const;

  const std::string tag_;
  std::string literal_;
protected:
  const FilterFactory handler_;
  const std::vector<CostFilterNode> args_;
};

class CostFilter {
public:
  // default filter just returns true (does nothing)
  CostFilter() = delete;

  CostFilter(const Costing& costing) : options_(&costing.options()), root_(costing.filter()) {}

  bool operator() (CostFilterArg& result, const DirectedEdge* edge, const CustomAttributes& attrs) const {
    return root_(*options_, result, edge, attrs);
  }

  bool operator() (const DirectedEdge* edge, const CustomAttributes& attrs) const {
    return root_(*options_, edge, attrs);
  }

  std::string to_string() const {
    return root_.to_string();
  }

  static Costing_Filter* ToPBF(const boost::property_tree::ptree& config, Costing_Type type) {
    return ToPBF(config, valhalla::Costing_Enum_Name(type));
  }

  static Costing_Filter* ToPBF(const boost::property_tree::ptree& config, std::string name) {
    const auto& expression = config.get_child_optional("sif.filters." + name);
    if (!expression || expression->empty()) {
      return nullptr;
    }
    return ToPBF(*expression);
  }

  static Costing_Filter* ToPBF(const boost::property_tree::ptree& expression) {
    auto *const filter = new Costing_Filter;
    ToPBF(expression, filter);
    return filter;
  }

  static void ToPBF(const boost::property_tree::ptree& expression, Costing_Filter* filter) {
    auto iter = expression.begin();
    auto end = expression.end();
    if (iter == end) {
      // TODO: error
      return;
    }
    // LOG_INFO("topbf filter: " + iter->first + ", " + iter->second.data());
    filter->set_tag(iter->second.data());
    std::advance(iter, 1);

    // auto value = expression.get_optional<std::string>(iter->first);
    // if (value && !value->empty()) {
    //   LOG_INFO("topbf value: " + *value);
    //   auto* child = filter->mutable_children()->Add();
    //   child->set_tag(*value);
    //   return;
    // }

    for(; iter != end; iter++) {
      // LOG_INFO("topbf iter: " + iter->first + ", " + iter->second.data());
      const auto value = iter->second.data();
      auto* child = filter->mutable_children()->Add();
      // auto value = expression.get_optional<std::string>(iter->first);
      if (!value.empty()) {
        child->set_tag("literal");
        child->set_data(value);
        continue;
      }
      if (iter->second.empty()) continue; // TODO: error
      ToPBF(iter->second, child);
    }
  }

  static void parse_filters(const boost::property_tree::ptree& config, Options& options) {
    LOG_INFO("parse_filters()");
    auto cost_type = options.costing_type();
    auto args = options.filter_args();
    Costing_Filter* filter;
    Costing_Filter* all_filter = sif::CostFilter::ToPBF(config, "all");

    std::stringstream ss;
    for (const auto& kv : args) {
      ss << kv.first << ": " << kv.second << ",";
    }
    LOG_INFO("filter_args:{" + ss.str() + "}");

    auto copy_args = [&args](Costing& costing) {
      if (!args.empty() && costing.options().filter_args().empty()) {
        auto& to_args = *costing.mutable_options()->mutable_filter_args();
        for (const auto& kv: args) {
          to_args[kv.first] = kv.second;
        }
      }
    };
    if (options.has_filter()) {
      filter = options.mutable_filter();
    } else {
      filter = sif::CostFilter::ToPBF(config, cost_type);
    }
    if (!options.costings_size()) return;
    for (auto &kv: *options.mutable_costings()) {
      const auto name = sif::kCostingNameMapping.find(static_cast<Costing_Type>(kv.first))->second;
      // if (name == sif::kCostingNameMapping.end()) {
      //   // TODO:error
      //   name = "";
      // }
      if (kv.second.has_filter()) {
        LOG_INFO("Cost filter for '" + name + " supplied in request");
        continue;
      }
      if (kv.first == cost_type && filter != nullptr) {
        LOG_INFO("Setting found cost filter for matching costing: " + name);
        kv.second.set_allocated_filter(filter);
        copy_args(kv.second);
      } else {
        auto* cost_filter = sif::CostFilter::ToPBF(config, name);
        if (cost_filter != nullptr) {
          LOG_INFO("Setting found cost filter: " + name);
          kv.second.set_allocated_filter(cost_filter);
          copy_args(kv.second);
        } else if (all_filter != nullptr) {
          LOG_INFO("Setting default cost filter: " + name);
          kv.second.set_allocated_filter(all_filter);
          copy_args(kv.second);
        }
      }
      if (kv.second.has_filter()) {
        LOG_WARN("Parsed filter for '" + name + "'");
      } else {
        LOG_WARN("Costing '" + name + "' has no filter");
      }
    }
  }

protected:
  const Costing_Options* options_;
  const CostFilterNode root_;
  const std::vector<CostFilter> args_;
};

struct Filters {
  static FilterHandler True(const Costing_Options& /*options*/, const CostFilterNode* /*self*/)  {
    return [](CostFilterArg& result,
              const DirectedEdge*  /*edge*/,
              const std::vector<CostFilterNode>&  /*args*/,
              const CustomAttributes& /*attrs*/) {
      result.value = "1";
    };
  }

  static FilterHandler False(const Costing_Options& /*options*/, const CostFilterNode* /*self*/) {
    return [](CostFilterArg& result,
              const DirectedEdge*  /*edge*/,
              const std::vector<CostFilterNode>&  /*args*/,
              const CustomAttributes& /*attrs*/) {
      result.value = "";
    };
  }

  static FilterHandler Literal (const Costing_Options& /*options*/, const CostFilterNode* self) {
    // LOG_INFO("returning literal inner");
    return [self](CostFilterArg& result,
                   const DirectedEdge* /*edge*/,
                   const std::vector<CostFilterNode>& /*args*/,
                   const CustomAttributes& /*attrs*/) {
      result.value = self->literal_;
      // result.value = "";
      LOG_INFO(self->to_string () + ": " + result.value);
    };
  }

  static FilterHandler Or(const Costing_Options& options, const CostFilterNode* self) {
    return [self,&options](CostFilterArg& result,
                      const DirectedEdge* edge,
                      const std::vector<CostFilterNode>& args,
                      const CustomAttributes& attrs) {
      for (const auto &arg: args) {
        arg(options, result, edge, attrs);
        if (!result.value.empty()) break; // return;
      }
      LOG_INFO(self->to_string () + ": " + result.value);
    };
  }

  static FilterHandler And(const Costing_Options& options, const CostFilterNode*  /*self*/) {
    return [&options](CostFilterArg& result,
                      const DirectedEdge* edge,
                      const std::vector<CostFilterNode>& args,
                      const CustomAttributes& attrs) {
      for (const auto &arg: args) {
        arg(options, result, edge, attrs);
        if (result.value.empty()) return;
      }
    };
  }

  static FilterHandler Not(const Costing_Options& options, const CostFilterNode* self) {
    return [self,&options](CostFilterArg& result,
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
      LOG_INFO(self->to_string () + ": " + result.value);
    };
  }

  static FilterHandler Eq(const Costing_Options& options, const CostFilterNode* self) {
    return [self,&options](CostFilterArg& result,
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
          // return;
          break;
        }
      }
      LOG_INFO(self->to_string () + ": " + result.value);
    };
  }

  static FilterHandler Request(const Costing_Options& options, const CostFilterNode* self) {
    return [self,&options](CostFilterArg& result,
                      const DirectedEdge* edge,
                      const std::vector<CostFilterNode>& args,
                      const CustomAttributes& attrs) {
      std::stringstream ss;
      for (const auto& kv : options.filter_args()) {
        ss << kv.first << ": " << kv.second << ",";
      }
      LOG_INFO("Request() filter_args:{" + ss.str() + "}");
      if (!args.size()) {
        // TODO:error or set result "null"
        result.value = "";
        return;
      }
      // get key
      args[0](options, result, edge, attrs);
      if (result.value.empty()) {
        // TODO:error
        return;
      }
      LOG_INFO("Request() key: " + std::string(result.value));
      for (const auto &kv: options.filter_args()) {
        if (kv.first == result.value) {
          result.value = kv.second;
          LOG_INFO(self->to_string () + ": " + result.value);
          return;
        }
      }
      // TODO:error?
      result.value = "";
      LOG_INFO(self->to_string () + ": " + result.value);
    };
  }

  static FilterHandler Get(const Costing_Options&  options, const CostFilterNode* self) {
    return [self,&options](CostFilterArg& result,
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
        // TODO:error
        return;
      }
      auto value = attrs.find(result.value);
      result.value = (value == attrs.cend() ? "" : value->second);
      LOG_INFO(self->to_string () + ": " + result.value);
    };
  }
};

const std::unordered_map<std::string, FilterFactory> filter_handlers = {
  {"true",    Filters::True},
  {"false",   Filters::False},
  {"literal", Filters::Literal},
  {"or",      Filters::Or},
  {"and",     Filters::And},
  {"not",     Filters::Not},
  {"!",       Filters::Not},
  {"==",      Filters::Eq},
  {"eq",      Filters::Eq},
  {"request", Filters::Request},
  {"get",     Filters::Get},
};

//   const std::unordered_map<FilterFactory, std::string> handler_names = {
//     {Filters::True,    "true"},
//     {Filters::False,   "false"},
//     {Filters::Literal, "literal"},
//     {Filters::Or,      "or"},
//     {Filters::And,     "and"},
//     {Filters::Not,     "not"},
//     {Filters::Not,     "!"},
//     {Filters::Eq,      "=="},
//     {Filters::Eq,      "eq"},
//     {Filters::Request ,"request"},
//     {Filters::Get,     "get"},
// };


FilterFactory get_filter_handler(const Costing_Filter& filter);

} // namespace sif

} // namespace valhalla

namespace std {
inline std::string to_string(const valhalla::sif::CostFilter& filter) {
  return filter.to_string();
}
}

#endif  //  VALHALLA_SIF_COSTFILTER_H_
