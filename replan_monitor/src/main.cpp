#include "rclcpp/rclcpp.hpp"
#include "replan_monitor/replan_monitor_node.hpp"

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);

  

  rclcpp::executors::MultiThreadedExecutor executor;
  auto node = std::make_shared<ReplanMonitorNode>();
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}