// Copyright (c) 2018 Intel Corporation
// Copyright (c) 2020 Florian Gramss
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NAV2_BEHAVIOR_TREE__BEHAVIOR_TREE_ENGINE_HPP_
#define NAV2_BEHAVIOR_TREE__BEHAVIOR_TREE_ENGINE_HPP_

#include <memory>
#include <string>
#include <vector>

#include "behaviortree_cpp/behavior_tree.h"
#include "behaviortree_cpp/bt_factory.h"
#include "behaviortree_cpp/xml_parsing.h"

#include "behaviortree_cpp/loggers/bt_cout_logger.h"  // ADD chang.gwak
#include "behaviortree_cpp/loggers/bt_file_logger_v2.h" // ADD chang.gwak

#include "rclcpp/rclcpp.hpp"

namespace nav2_behavior_tree
{

/**
 * @enum nav2_behavior_tree::BtStatus
 * @brief An enum class representing BT execution status
 */
enum class BtStatus { SUCCEEDED, FAILED, CANCELED };

/**
 * @class nav2_behavior_tree::BehaviorTreeEngine
 * @brief A class to create and handle behavior trees
 */
class BehaviorTreeEngine
{
public:
  /**
   * @brief A constructor for nav2_behavior_tree::BehaviorTreeEngine
   * @param plugin_libraries vector of BT plugin library names to load
   */
  explicit BehaviorTreeEngine(
    const std::vector<std::string> & plugin_libraries,
    rclcpp::Node::SharedPtr node);
  virtual ~BehaviorTreeEngine() {}

  /**
   * @brief Function to execute a BT at a specific rate
   * @param tree BT to execute
   * @param onLoop Function to execute on each iteration of BT execution
   * @param cancelRequested Function to check if cancel was requested during BT execution
   * @param loopTimeout Time period for each iteration of BT execution
   * @return nav2_behavior_tree::BtStatus Status of BT execution
   */
  BtStatus run(
    BT::Tree * tree,
    std::function<void()> onLoop,
    std::function<bool()> cancelRequested,
    std::chrono::milliseconds loopTimeout = std::chrono::milliseconds(10));

  /**
   * @brief Function to create a BT from a XML string
   * @param xml_string XML string representing BT
   * @param blackboard Blackboard for BT
   * @return BT::Tree Created behavior tree
   */
  BT::Tree createTreeFromText(
    const std::string & xml_string,
    BT::Blackboard::Ptr blackboard);

  /**
   * @brief Function to create a BT from an XML file
   * @param file_path Path to BT XML file
   * @param blackboard Blackboard for BT
   * @return BT::Tree Created behavior tree
   */
  BT::Tree createTreeFromFile(
    const std::string & file_path,
    BT::Blackboard::Ptr blackboard);

    // ADD chang.gwak
  /**
   * @brief Function to attach a standard cout logger to the BT
   */
  void addStdCoutLogger(const BT::Tree &tree);
  // void addStdCoutLogger(
  //   const BT::Tree &tree, 
  //   const std::vector<std::string> & nodes_to_log = {});


  void addFileLogger2(const BT::Tree &tree, const std::string & filepath);
    /////////////// done ADD chang.gwak

  /**
   * @brief Function to explicitly reset all BT nodes to initial state
   * @param tree Tree to halt
   */
  void haltAllActions(BT::Tree & tree);

protected:
  // The factory that will be used to dynamically construct the behavior tree
  BT::BehaviorTreeFactory factory_;

  // Clock
  rclcpp::Clock::SharedPtr clock_;

  // ADD chang.gwak
  // Logger
  std::unique_ptr<BT::StdCoutLogger> cout_logger_;
  // std::unique_ptr<BT::StatusChangeLogger> cout_logger_;
  std::unique_ptr<BT::FileLogger2> file_logger2_;
    /////////////// done ADD chang.gwak

};

}  // namespace nav2_behavior_tree

#endif  // NAV2_BEHAVIOR_TREE__BEHAVIOR_TREE_ENGINE_HPP_
