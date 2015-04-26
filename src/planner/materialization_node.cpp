/**
 * @brief Implementation of materialization plan node.
 *
 * Copyright(c) 2015, CMU
 */

#include "planner/materialization_node.h"

#include <utility>

#include "catalog/schema.h"

namespace nstore {
namespace planner {

MaterializationNode::MaterializationNode(
    std::unordered_map<id_t, id_t> &&old_to_new_cols,
    catalog::Schema *schema)
  : old_to_new_cols_(std::move(old_to_new_cols)),
    schema_(schema) {
}

const std::unordered_map<id_t, id_t>&
MaterializationNode::old_to_new_cols() const {
  return old_to_new_cols_;
}

const catalog::Schema& MaterializationNode::schema() const {
  return *schema_;
}

PlanNodeType MaterializationNode::GetPlanNodeType() const {
  //TODO Implement.
  return PLAN_NODE_TYPE_INVALID;
}

std::string MaterializationNode::debugInfo(const std::string& spacer) const {
  (void) spacer;
  //TODO Implement.
  return "";
}

} // namespace planner
} // namespace nstore