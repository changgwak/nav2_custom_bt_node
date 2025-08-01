#include "path_obstacle_filter/path_obstacle_filter_node.hpp"

#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>
#include "tf2_sensor_msgs/tf2_sensor_msgs.hpp"
#include "rclcpp_components/register_node_macro.hpp"

namespace path_obstacle_filter
{

PathObstacleFilterNode::PathObstacleFilterNode(const rclcpp::NodeOptions & options)
: Node("path_obstacle_filter", options)
{
  RCLCPP_INFO(this->get_logger(), "Initializing PathObstacleFilterNode...");

  this->getParameters();

  tf_buffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
  tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

  auto default_qos = rclcpp::QoS(rclcpp::SystemDefaultsQoS());
  path_sub_ = this->create_subscription<nav_msgs::msg::Path>(
    "/plan", default_qos, std::bind(&PathObstacleFilterNode::pathCallback, this, std::placeholders::_1));

  goal_status_sub_ = this->create_subscription<action_msgs::msg::GoalStatusArray>(
    "/navigate_to_pose/_action/status", rclcpp::SystemDefaultsQoS(),
    std::bind(&PathObstacleFilterNode::goalStatusCallback, this, std::placeholders::_1));

  std::string sensor_type = this->get_parameter("sensor_topic_type").as_string();
  if (sensor_type == "scan") {
    RCLCPP_INFO(this->get_logger(), "Subscribing to LaserScan topic: /scan");
    scan_sub_ = this->create_subscription<sensor_msgs::msg::LaserScan>(
      "/scan", rclcpp::SensorDataQoS(), std::bind(&PathObstacleFilterNode::scanCallback, this, std::placeholders::_1));
  } else if (sensor_type == "pointcloud") {
    RCLCPP_INFO(this->get_logger(), "Subscribing to PointCloud2 topic: /pointcloud");
    pointcloud_sub_ = this->create_subscription<sensor_msgs::msg::PointCloud2>(
      "/pointcloud", rclcpp::SensorDataQoS(), std::bind(&PathObstacleFilterNode::pointcloudCallback, this, std::placeholders::_1));
  }

  filtered_cloud_pub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>(
    "/filtered_cloud", default_qos);

  timer_ = this->create_wall_timer(
    std::chrono::milliseconds(100), std::bind(&PathObstacleFilterNode::process, this));

  RCLCPP_INFO(this->get_logger(), "PathObstacleFilterNode initialized successfully.");
}

void PathObstacleFilterNode::getParameters()
{
  this->declare_parameter("global_frame", "map");
  this->declare_parameter("robot_base_frame", "base_link");
  this->declare_parameter("sensor_topic_type", "scan");
  this->declare_parameter("distance_threshold", 0.3);
  this->declare_parameter("lookahead_dist", 5.0);
  this->declare_parameter("goal_tolerance", 0.25);
  this->declare_parameter("enable_pre_inflation", true);
  this->declare_parameter("pre_inflation_radius", 0.4);
  this->declare_parameter("inflation_points", 8);
  this->declare_parameter("enable_data_merging", true); // 추가

  global_frame_ = this->get_parameter("global_frame").as_string();
  robot_base_frame_ = this->get_parameter("robot_base_frame").as_string();
  distance_threshold_ = this->get_parameter("distance_threshold").as_double();
  lookahead_dist_ = this->get_parameter("lookahead_dist").as_double();
  goal_tolerance_ = this->get_parameter("goal_tolerance").as_double();
  enable_pre_inflation_ = this->get_parameter("enable_pre_inflation").as_bool();
  pre_inflation_radius_ = this->get_parameter("pre_inflation_radius").as_double();
  inflation_points_ = this->get_parameter("inflation_points").as_int();
  enable_data_merging_ = this->get_parameter("enable_data_merging").as_bool(); // 추가
}

void PathObstacleFilterNode::goalStatusCallback(const action_msgs::msg::GoalStatusArray::SharedPtr msg)
{
  if (msg->status_list.empty()) {
    is_goal_active_ = false;
    return;
  }
  const auto & latest_goal_status = msg->status_list.back();
  int status = latest_goal_status.status;
  if (status == action_msgs::msg::GoalStatus::STATUS_ACCEPTED ||
      status == action_msgs::msg::GoalStatus::STATUS_EXECUTING)
  {
    is_goal_active_ = true;
  } else {
    is_goal_active_ = false;
  }
}



void PathObstacleFilterNode::pathCallback(const nav_msgs::msg::Path::SharedPtr msg)
{
  std::lock_guard<std::mutex> lock(data_mutex_);
  current_path_ = *msg;
  path_received_ = true;
}

void PathObstacleFilterNode::scanCallback(const sensor_msgs::msg::LaserScan::SharedPtr msg)
{
  if (!tf_buffer_->canTransform(
      global_frame_, msg->header.frame_id, msg->header.stamp,
      rclcpp::Duration::from_seconds(0.1)))
  {
    RCLCPP_WARN(
      this->get_logger(), "Transform from %s to %s not available yet.",
      msg->header.frame_id.c_str(), global_frame_.c_str());
    return;
  }

  sensor_msgs::msg::PointCloud2 cloud_msg;
  try {
    projector_.transformLaserScanToPointCloud(global_frame_, *msg, cloud_msg, *tf_buffer_);
  } catch (const tf2::TransformException & ex) {
    RCLCPP_ERROR(this->get_logger(), "Unexpected transform error after waiting: %s", ex.what());
    return;
  }

  std::lock_guard<std::mutex> lock(data_mutex_);
  pcl::fromROSMsg(cloud_msg, latest_cloud_);
  cloud_received_ = true;
}

void PathObstacleFilterNode::pointcloudCallback(const sensor_msgs::msg::PointCloud2::SharedPtr msg)
{
  if (msg->header.frame_id != global_frame_) {
    geometry_msgs::msg::TransformStamped transform;
    try {
      transform = tf_buffer_->lookupTransform(global_frame_, msg->header.frame_id, msg->header.stamp, rclcpp::Duration::from_seconds(0.1));
      sensor_msgs::msg::PointCloud2 transformed_cloud;
      tf2::doTransform(*msg, transformed_cloud, transform);
      std::lock_guard<std::mutex> lock(data_mutex_);
      pcl::fromROSMsg(transformed_cloud, latest_cloud_);
    } catch (const tf2::TransformException & ex) {
      RCLCPP_WARN(this->get_logger(), "Could not transform pointcloud to %s: %s", global_frame_.c_str(), ex.what());
      return;
    }
  } else {
    std::lock_guard<std::mutex> lock(data_mutex_);
    pcl::fromROSMsg(*msg, latest_cloud_);
  }
  cloud_received_ = true;
}


bool PathObstacleFilterNode::getRobotPose(geometry_msgs::msg::PoseStamped & pose)
{
  try {
    geometry_msgs::msg::TransformStamped t;
    t = tf_buffer_->lookupTransform(global_frame_, robot_base_frame_, tf2::TimePointZero);
    pose.header = t.header;
    pose.pose.position.x = t.transform.translation.x;
    pose.pose.position.y = t.transform.translation.y;
    pose.pose.position.z = t.transform.translation.z;
    pose.pose.orientation = t.transform.rotation;
    return true;
  } catch (const tf2::TransformException & ex) {
    RCLCPP_WARN(this->get_logger(), "Could not get robot pose: %s", ex.what());
    return false;
  }
}

double PathObstacleFilterNode::pointToPathSegmentDistance(
  const pcl::PointXYZ & point, const geometry_msgs::msg::Point & p1, const geometry_msgs::msg::Point & p2)
{
  double l2 = (p2.x - p1.x) * (p2.x - p1.x) + (p2.y - p1.y) * (p2.y - p1.y);
  if (l2 == 0.0) return std::hypot(point.x - p1.x, point.y - p1.y);
  double t = ((point.x - p1.x) * (p2.x - p1.x) + (point.y - p1.y) * (p2.y - p1.y)) / l2;
  t = std::max(0.0, std::min(1.0, t));
  double proj_x = p1.x + t * (p2.x - p1.x);
  double proj_y = p1.y + t * (p2.y - p1.y);
  return std::hypot(point.x - proj_x, point.y - proj_y);
}



void PathObstacleFilterNode::process()
{
  // 1. 골 활성 상태 확인: Nav2의 목표가 활성 상태인지 먼저 확인합니다.
  if (!is_goal_active_) {
    // 골이 비활성 상태일 때의 처리
    if (path_received_) {
      RCLCPP_INFO(this->get_logger(), "Goal is not active. Reverting to raw sensor data passthrough.");
      std::lock_guard<std::mutex> lock(data_mutex_);
      current_path_.poses.clear();
      path_received_ = false;
    }

    // 원본 센서 데이터를 그대로 발행하여 코스트맵의 'clearing'을 활성화시킴
    std::lock_guard<std::mutex> lock(data_mutex_);
    if (cloud_received_) {
      pcl::PointCloud<pcl::PointXYZ> raw_cloud = latest_cloud_;
      
      pcl_conversions::toPCL(this->now(), raw_cloud.header.stamp);
      raw_cloud.header.frame_id = global_frame_;

      sensor_msgs::msg::PointCloud2 output_msg;
      pcl::toROSMsg(raw_cloud, output_msg);
      filtered_cloud_pub_->publish(output_msg);
    }
    return;
  }

  // --- 아래는 골이 활성 상태일 때만 실행되는 로직 ---

  // 2. 데이터 수신 및 로봇 포즈 확인
  if (!path_received_ || !cloud_received_) { return; }
  geometry_msgs::msg::PoseStamped robot_pose;
  if (!getRobotPose(robot_pose)) { return; }

  // 3. 스레드 안전하게 데이터 복사 및 골 도달 확인
  nav_msgs::msg::Path path;
  pcl::PointCloud<pcl::PointXYZ> original_cloud;
  {
    std::lock_guard<std::mutex> lock(data_mutex_);
    if (current_path_.poses.empty()) { path_received_ = false; return; }
    const auto & goal_pose = current_path_.poses.back().pose;
    double dist_to_goal = std::hypot(
      goal_pose.position.x - robot_pose.pose.position.x,
      goal_pose.position.y - robot_pose.pose.position.y);

    if (dist_to_goal < goal_tolerance_) {
      RCLCPP_INFO(this->get_logger(), "Goal reached. Clearing path.");
      current_path_.poses.clear();
      path_received_ = false;
      is_goal_active_ = false;
      return;
    }
    path = current_path_;
    original_cloud = latest_cloud_;
  }

  // [디버깅 로그 1] 원본 센서 데이터가 몇 개의 포인트로 들어오는지 확인
  RCLCPP_INFO_THROTTLE(
    this->get_logger(), *this->get_clock(), 2000, 
    "Processing... Original cloud size: %zu", original_cloud.points.size());

  // 4. 경로 주변 장애물 필터링
  pcl::PointCloud<pcl::PointXYZ> filtered_points;
  size_t closest_path_idx = 0;
  double min_dist_sq = std::numeric_limits<double>::max();
  for (size_t i = 0; i < path.poses.size(); ++i) {
    double dist_sq = (path.poses[i].pose.position.x - robot_pose.pose.position.x) * (path.poses[i].pose.position.x - robot_pose.pose.position.x) +
                     (path.poses[i].pose.position.y - robot_pose.pose.position.y) * (path.poses[i].pose.position.y - robot_pose.pose.position.y);
    if (dist_sq < min_dist_sq) {
      min_dist_sq = dist_sq;
      closest_path_idx = i;
    }
  }

  for (const auto & point : original_cloud.points) {
    double min_dist_to_path = std::numeric_limits<double>::max();
    for (size_t i = closest_path_idx; i < path.poses.size() - 1; ++i) {
      const auto & p1 = path.poses[i].pose.position;
      const auto & p2 = path.poses[i + 1].pose.position;
      double dist_to_segment_start = std::hypot(p1.x - robot_pose.pose.position.x, p1.y - robot_pose.pose.position.y);
      if (dist_to_segment_start > lookahead_dist_) { break; }
      double dist = pointToPathSegmentDistance(point, p1, p2);
      if (dist < min_dist_to_path) { min_dist_to_path = dist; }
    }
    if (min_dist_to_path <= distance_threshold_) {
      filtered_points.points.push_back(point);
    }
  }

  //  [디버깅 로그 2] 필터링 후 몇 개의 포인트가 살아남았는지 확인 (가장 중요!)
  RCLCPP_INFO_THROTTLE(
    this->get_logger(), *this->get_clock(), 2000, 
    "Filtered points size: %zu", filtered_points.points.size());


  // 5. 필터링된 장애물에 대해서만 인플레이션 포인트 생성
  pcl::PointCloud<pcl::PointXYZ> inflation_points;
  if (enable_pre_inflation_ && pre_inflation_radius_ > 0.0 && !filtered_points.empty()) {
    inflation_points.points.reserve(filtered_points.points.size() * inflation_points_);
    for (const auto & point : filtered_points.points) {
      for (int i = 0; i < inflation_points_; ++i) {
        double angle = 2.0 * M_PI * i / inflation_points_;
        pcl::PointXYZ inflation_point;
        inflation_point.x = point.x + pre_inflation_radius_ * cos(angle);
        inflation_point.y = point.y + pre_inflation_radius_ * sin(angle);
        inflation_point.z = point.z;
        inflation_points.points.push_back(inflation_point);
      }
    }
  }

  // 6. 최종 발행할 클라우드를 파라미터에 따라 결정
  pcl::PointCloud<pcl::PointXYZ> cloud_to_publish;
  if (enable_data_merging_) {
    cloud_to_publish = original_cloud;
    cloud_to_publish += inflation_points;
  } else {
    cloud_to_publish = filtered_points;
    cloud_to_publish += inflation_points;
  }
  
  // [디버깅 로그 3] 최종적으로 발행될 포인트 클라우드의 크기 확인
  RCLCPP_INFO_THROTTLE(
    this->get_logger(), *this->get_clock(), 2000, 
    "Publishing cloud with %zu points.", cloud_to_publish.points.size());

  // 7. 안정적인 방식으로 최종 클라우드 발행
  pcl_conversions::toPCL(this->now(), cloud_to_publish.header.stamp);
  cloud_to_publish.header.frame_id = global_frame_;

  sensor_msgs::msg::PointCloud2 output_msg;
  pcl::toROSMsg(cloud_to_publish, output_msg);
  filtered_cloud_pub_->publish(output_msg);
}

}  // namespace path_obstacle_filter

RCLCPP_COMPONENTS_REGISTER_NODE(path_obstacle_filter::PathObstacleFilterNode)



























// #include "path_obstacle_filter/path_obstacle_filter_node.hpp"

// #include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>
// #include "tf2_sensor_msgs/tf2_sensor_msgs.hpp"
// #include "rclcpp_components/register_node_macro.hpp"

// namespace path_obstacle_filter
// {

// PathObstacleFilterNode::PathObstacleFilterNode(const rclcpp::NodeOptions & options)
// : Node("path_obstacle_filter", options)
// {
//   RCLCPP_INFO(this->get_logger(), "Initializing PathObstacleFilterNode...");

//   this->getParameters();

//   tf_buffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
//   tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

//   auto default_qos = rclcpp::QoS(rclcpp::SystemDefaultsQoS());
//   path_sub_ = this->create_subscription<nav_msgs::msg::Path>(
//     "/plan", default_qos,
//     std::bind(&PathObstacleFilterNode::pathCallback, this, std::placeholders::_1));

//   std::string sensor_type = this->get_parameter("sensor_topic_type").as_string();
//   if (sensor_type == "scan") {
//     RCLCPP_INFO(this->get_logger(), "Subscribing to LaserScan topic: /scan");
//     scan_sub_ = this->create_subscription<sensor_msgs::msg::LaserScan>(
//       "/scan", rclcpp::SensorDataQoS(),
//       std::bind(&PathObstacleFilterNode::scanCallback, this, std::placeholders::_1));
//   } else if (sensor_type == "pointcloud") {
//     RCLCPP_INFO(this->get_logger(), "Subscribing to PointCloud2 topic: /pointcloud");
//     pointcloud_sub_ = this->create_subscription<sensor_msgs::msg::PointCloud2>(
//       "/pointcloud", rclcpp::SensorDataQoS(),
//       std::bind(&PathObstacleFilterNode::pointcloudCallback, this, std::placeholders::_1));
//   } else {
//     RCLCPP_ERROR(this->get_logger(), "Invalid sensor_topic_type: %s. Use 'scan' or 'pointcloud'", sensor_type.c_str());
//   }

//   filtered_cloud_pub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>(
//     "/filtered_cloud", default_qos);

//   timer_ = this->create_wall_timer(
//     std::chrono::milliseconds(100), // 10 Hz
//     std::bind(&PathObstacleFilterNode::process, this));

//   RCLCPP_INFO(this->get_logger(), "PathObstacleFilterNode initialized successfully.");
// }

// void PathObstacleFilterNode::getParameters()
// {
//   this->declare_parameter("global_frame", "map");
//   this->declare_parameter("robot_base_frame", "base_link");
//   this->declare_parameter("sensor_topic_type", "scan");
//   this->declare_parameter("distance_threshold", 0.5);
//   this->declare_parameter("lookahead_dist", 2.0);
//   this->declare_parameter("goal_tolerance", 0.25);
//   this->declare_parameter("enable_pre_inflation", true);
//   this->declare_parameter("pre_inflation_radius", 0.4); //0.1
//   this->declare_parameter("inflation_points", 20);  // 6

//   global_frame_ = this->get_parameter("global_frame").as_string();
//   robot_base_frame_ = this->get_parameter("robot_base_frame").as_string();
//   distance_threshold_ = this->get_parameter("distance_threshold").as_double();
//   lookahead_dist_ = this->get_parameter("lookahead_dist").as_double();
//   goal_tolerance_ = this->get_parameter("goal_tolerance").as_double();
//   enable_pre_inflation_ = this->get_parameter("enable_pre_inflation").as_bool();
//   pre_inflation_radius_ = this->get_parameter("pre_inflation_radius").as_double();
//   inflation_points_ = this->get_parameter("inflation_points").as_int();
// }

// void PathObstacleFilterNode::pathCallback(const nav_msgs::msg::Path::SharedPtr msg)
// {
//   std::lock_guard<std::mutex> lock(data_mutex_);
//   current_path_ = *msg;
//   path_received_ = true;
// }

// void PathObstacleFilterNode::scanCallback(const sensor_msgs::msg::LaserScan::SharedPtr msg)
// {
//   if (!tf_buffer_->canTransform(
//       global_frame_, msg->header.frame_id, msg->header.stamp,
//       rclcpp::Duration::from_seconds(0.1)))
//   {
//     RCLCPP_WARN(
//       this->get_logger(), "Transform from %s to %s not available yet.",
//       msg->header.frame_id.c_str(), global_frame_.c_str());
//     return;
//   }

//   sensor_msgs::msg::PointCloud2 cloud_msg;
//   try {
//     projector_.transformLaserScanToPointCloud(global_frame_, *msg, cloud_msg, *tf_buffer_);
//   } catch (const tf2::TransformException & ex) {
//     RCLCPP_ERROR(this->get_logger(), "Unexpected transform error after waiting: %s", ex.what());
//     return;
//   }

//   std::lock_guard<std::mutex> lock(data_mutex_);
//   pcl::fromROSMsg(cloud_msg, latest_cloud_);
//   cloud_received_ = true;
// }

// void PathObstacleFilterNode::pointcloudCallback(const sensor_msgs::msg::PointCloud2::SharedPtr msg)
// {
//   if (msg->header.frame_id != global_frame_) {
//     geometry_msgs::msg::TransformStamped transform;
//     try {
//       transform = tf_buffer_->lookupTransform(global_frame_, msg->header.frame_id, msg->header.stamp, rclcpp::Duration::from_seconds(0.1));
//       sensor_msgs::msg::PointCloud2 transformed_cloud;
//       tf2::doTransform(*msg, transformed_cloud, transform);
//       std::lock_guard<std::mutex> lock(data_mutex_);
//       pcl::fromROSMsg(transformed_cloud, latest_cloud_);
//     } catch (const tf2::TransformException & ex) {
//       RCLCPP_WARN(this->get_logger(), "Could not transform pointcloud to %s: %s", global_frame_.c_str(), ex.what());
//       return;
//     }
//   } else {
//     std::lock_guard<std::mutex> lock(data_mutex_);
//     pcl::fromROSMsg(*msg, latest_cloud_);
//   }
//   cloud_received_ = true;
// }


// bool PathObstacleFilterNode::getRobotPose(geometry_msgs::msg::PoseStamped & pose)
// {
//   try {
//     geometry_msgs::msg::TransformStamped t;
//     t = tf_buffer_->lookupTransform(global_frame_, robot_base_frame_, tf2::TimePointZero);
//     pose.header = t.header;
//     pose.pose.position.x = t.transform.translation.x;
//     pose.pose.position.y = t.transform.translation.y;
//     pose.pose.position.z = t.transform.translation.z;
//     pose.pose.orientation = t.transform.rotation;
//     return true;
//   } catch (const tf2::TransformException & ex) {
//     RCLCPP_WARN(this->get_logger(), "Could not get robot pose: %s", ex.what());
//     return false;
//   }
// }

// double PathObstacleFilterNode::pointToPathSegmentDistance(
//   const pcl::PointXYZ & point, const geometry_msgs::msg::Point & p1, const geometry_msgs::msg::Point & p2)
// {
//   double l2 = (p2.x - p1.x) * (p2.x - p1.x) + (p2.y - p1.y) * (p2.y - p1.y);
//   if (l2 == 0.0) return std::hypot(point.x - p1.x, point.y - p1.y);
//   double t = ((point.x - p1.x) * (p2.x - p1.x) + (point.y - p1.y) * (p2.y - p1.y)) / l2;
//   t = std::max(0.0, std::min(1.0, t));
//   double proj_x = p1.x + t * (p2.x - p1.x);
//   double proj_y = p1.y + t * (p2.y - p1.y);
//   return std::hypot(point.x - proj_x, point.y - proj_y);
// }

// void PathObstacleFilterNode::process()
// {
//   if (!path_received_ || !cloud_received_) {
//     return;
//   }

//   geometry_msgs::msg::PoseStamped robot_pose;
//   if (!getRobotPose(robot_pose)) {
//     return;
//   }

//   nav_msgs::msg::Path path;
//   pcl::PointCloud<pcl::PointXYZ> cloud;

//   {
//     std::lock_guard<std::mutex> lock(data_mutex_);
//     if (current_path_.poses.empty()) {
//       path_received_ = false;
//       return;
//     }
//     const auto & goal_pose = current_path_.poses.back().pose;
//     double dist_to_goal = std::hypot(
//       goal_pose.position.x - robot_pose.pose.position.x,
//       goal_pose.position.y - robot_pose.pose.position.y);

//     if (dist_to_goal < goal_tolerance_) {
//       RCLCPP_INFO(this->get_logger(), "Goal reached. Clearing path and stopping filter.");
//       current_path_.poses.clear();
//       path_received_ = false;
//       filtered_cloud_pub_->publish(sensor_msgs::msg::PointCloud2());
//       return;
//     }
//     path = current_path_;
//     cloud = latest_cloud_;
//   }

//   size_t closest_path_idx = 0;
//   double min_dist_sq = std::numeric_limits<double>::max();
//   for (size_t i = 0; i < path.poses.size(); ++i) {
//     double dist_sq = (path.poses[i].pose.position.x - robot_pose.pose.position.x) * (path.poses[i].pose.position.x - robot_pose.pose.position.x) +
//                      (path.poses[i].pose.position.y - robot_pose.pose.position.y) * (path.poses[i].pose.position.y - robot_pose.pose.position.y);
//     if (dist_sq < min_dist_sq) {
//       min_dist_sq = dist_sq;
//       closest_path_idx = i;
//     }
//   }

//   pcl::PointCloud<pcl::PointXYZ> filtered_cloud;
//   for (const auto & point : cloud.points) {
//     double min_dist_to_path = std::numeric_limits<double>::max();
//     for (size_t i = closest_path_idx; i < path.poses.size() - 1; ++i) {
//       const auto & p1 = path.poses[i].pose.position;
//       const auto & p2 = path.poses[i + 1].pose.position;
//       double dist_to_segment_start = std::hypot(p1.x - robot_pose.pose.position.x, p1.y - robot_pose.pose.position.y);
//       if (dist_to_segment_start > lookahead_dist_) {
//         break;
//       }
//       double dist = pointToPathSegmentDistance(point, p1, p2);
//       if (dist < min_dist_to_path) {
//         min_dist_to_path = dist;
//       }
//     }
//     if (min_dist_to_path <= distance_threshold_) {
//       filtered_cloud.points.push_back(point);
//     }
//   }

//   pcl::PointCloud<pcl::PointXYZ> cloud_to_publish;
//   if (enable_pre_inflation_ && pre_inflation_radius_ > 0.0) {
//     for (const auto & point : filtered_cloud.points) {
//       cloud_to_publish.points.push_back(point);
//       for (int i = 0; i < inflation_points_; ++i) {
//         double angle = 2.0 * M_PI * i / inflation_points_;
//         pcl::PointXYZ inflation_point;
//         inflation_point.x = point.x + pre_inflation_radius_ * cos(angle);
//         inflation_point.y = point.y + pre_inflation_radius_ * sin(angle);
//         inflation_point.z = point.z;
//         cloud_to_publish.points.push_back(inflation_point);
//       }
//     }
//   } else {
//     cloud_to_publish = filtered_cloud;
//   }
  
//   pcl_conversions::toPCL(this->now(), cloud_to_publish.header.stamp);
//   cloud_to_publish.header.frame_id = global_frame_;

//   sensor_msgs::msg::PointCloud2 output_msg;
//   pcl::toROSMsg(cloud_to_publish, output_msg);

//   filtered_cloud_pub_->publish(output_msg);
// }

// }  // namespace path_obstacle_filter

// RCLCPP_COMPONENTS_REGISTER_NODE(path_obstacle_filter::PathObstacleFilterNode)