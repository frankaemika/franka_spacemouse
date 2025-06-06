// Copyright (c) 2025 Franka Robotics GmbH
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

#include <franka_single_arm_controllers/default_robot_behavior_utils.hpp>
#include <franka_single_arm_controllers/joint_impedance_ik_controller.hpp>

#include <chrono>
#include <string>

using namespace std::chrono_literals;
using Vector7d = Eigen::Matrix<double, 7, 1>;

namespace franka_single_arm_controllers {

controller_interface::InterfaceConfiguration
JointImpedanceIKController::command_interface_configuration() const {
  controller_interface::InterfaceConfiguration config;
  config.type = controller_interface::interface_configuration_type::INDIVIDUAL;
  for (int i = 1; i <= num_joints_; ++i) {
    config.names.push_back(arm_id_ + "_joint" + std::to_string(i) + "/effort");
  }
  return config;
}

controller_interface::InterfaceConfiguration
JointImpedanceIKController::state_interface_configuration() const {
  controller_interface::InterfaceConfiguration config;
  config.type = controller_interface::interface_configuration_type::INDIVIDUAL;
  config.names = franka_cartesian_pose_->get_state_interface_names();
  for (int i = 1; i <= num_joints_; ++i) {
    config.names.push_back(arm_id_ + "_joint" + std::to_string(i) + "/position");
  }
  for (int i = 1; i <= num_joints_; ++i) {
    config.names.push_back(arm_id_ + "_joint" + std::to_string(i) + "/velocity");
  }
  for (int i = 1; i <= num_joints_; ++i) {
    config.names.push_back(arm_id_ + "_joint" + std::to_string(i) + "/effort");
  }
  for (const auto& franka_robot_model_name : franka_robot_model_->get_state_interface_names()) {
    config.names.push_back(franka_robot_model_name);
  }

  return config;
}

void JointImpedanceIKController::update_joint_states() {
  for (auto i = 0; i < num_joints_; ++i) {
    const auto& position_interface = state_interfaces_.at(16 + i);
    const auto& velocity_interface = state_interfaces_.at(23 + i);
    const auto& effort_interface = state_interfaces_.at(30 + i);
    joint_positions_current_[i] = position_interface.get_value();
    joint_velocities_current_[i] = velocity_interface.get_value();
    joint_efforts_current_[i] = effort_interface.get_value();
  }
}

std::shared_ptr<moveit_msgs::srv::GetPositionIK::Request>
JointImpedanceIKController::create_ik_service_request(
    const Eigen::Vector3d& position,
    const Eigen::Quaterniond& orientation,
    const std::vector<double>& joint_positions_current,
    const std::vector<double>& joint_velocities_current,
    const std::vector<double>& joint_efforts_current) {
  auto service_request = std::make_shared<moveit_msgs::srv::GetPositionIK::Request>();

  service_request->ik_request.group_name = arm_id_ + "_arm";
  service_request->ik_request.pose_stamped.header.frame_id = arm_id_ + "_link0";
  service_request->ik_request.pose_stamped.pose.position.x = position.x();
  service_request->ik_request.pose_stamped.pose.position.y = position.y();
  service_request->ik_request.pose_stamped.pose.position.z = position.z();
  service_request->ik_request.pose_stamped.pose.orientation.x = orientation.x();
  service_request->ik_request.pose_stamped.pose.orientation.y = orientation.y();
  service_request->ik_request.pose_stamped.pose.orientation.z = orientation.z();
  service_request->ik_request.pose_stamped.pose.orientation.w = orientation.w();
  service_request->ik_request.robot_state.joint_state.name = {
      arm_id_ + "_joint1", arm_id_ + "_joint2", arm_id_ + "_joint3", arm_id_ + "_joint4",
      arm_id_ + "_joint5", arm_id_ + "_joint6", arm_id_ + "_joint7"};
  service_request->ik_request.robot_state.joint_state.position = joint_positions_current;
  service_request->ik_request.robot_state.joint_state.velocity = joint_velocities_current;
  service_request->ik_request.robot_state.joint_state.effort = joint_efforts_current;

  if (is_gripper_loaded_) {
    service_request->ik_request.ik_link_name = arm_id_ + "_hand_tcp";
  }
  return service_request;
}

Vector7d JointImpedanceIKController::compute_torque_command(
    const Vector7d& joint_positions_desired,
    const Vector7d& joint_positions_current,
    const Vector7d& joint_velocities_current) {
  std::array<double, 7> coriolis_array = franka_robot_model_->getCoriolisForceVector();
  Vector7d coriolis(coriolis_array.data());
  const double kAlpha = 0.99;
  dq_filtered_ = (1 - kAlpha) * dq_filtered_ + kAlpha * joint_velocities_current;
  Vector7d q_error = joint_positions_desired - joint_positions_current;
  Vector7d tau_d_calculated =
      k_gains_.cwiseProduct(q_error) - d_gains_.cwiseProduct(dq_filtered_) + coriolis;

  return tau_d_calculated;
}

controller_interface::return_type JointImpedanceIKController::update(
    const rclcpp::Time& /*time*/,
    const rclcpp::Duration& /*period*/) {
  update_joint_states();
  std::tie(orientation_, position_) = franka_cartesian_pose_->getCurrentOrientationAndTranslation();

  auto new_position = position_ + desired_linear_position_update_;
  auto new_orientation = orientation_ * desired_angular_position_update_quaternion_;
  auto service_request =
      create_ik_service_request(new_position, new_orientation, joint_positions_current_,
                                joint_velocities_current_, joint_efforts_current_);

  using ServiceResponseFuture = rclcpp::Client<moveit_msgs::srv::GetPositionIK>::SharedFuture;
  auto response_received_callback =
      [&](ServiceResponseFuture future) {  // NOLINT(performance-unnecessary-value-param)
        const auto& response = future.get();

        if (response->error_code.val == response->error_code.SUCCESS) {
          joint_positions_desired_ = response->solution.joint_state.position;
        } else {
          RCLCPP_INFO(get_node()->get_logger(), "Inverse kinematics solution failed.");
        }
      };
  auto result_future_ =
      compute_ik_client_->async_send_request(service_request, response_received_callback);

  if (joint_positions_desired_.empty()) {
    return controller_interface::return_type::OK;
  }

  Vector7d joint_positions_desired_eigen(joint_positions_desired_.data());
  Vector7d joint_positions_current_eigen(joint_positions_current_.data());
  Vector7d joint_velocities_current_eigen(joint_velocities_current_.data());

  auto tau_d_calculated = compute_torque_command(
      joint_positions_desired_eigen, joint_positions_current_eigen, joint_velocities_current_eigen);

  for (int i = 0; i < num_joints_; i++) {
    command_interfaces_[i].set_value(tau_d_calculated(i));
  }

  return controller_interface::return_type::OK;
}

CallbackReturn JointImpedanceIKController::on_init() {
  franka_cartesian_pose_ =
      std::make_unique<franka_semantic_components::FrankaCartesianPoseInterface>(
          franka_semantic_components::FrankaCartesianPoseInterface(k_elbow_activated_));

  return CallbackReturn::SUCCESS;
}

bool JointImpedanceIKController::assign_parameters() {
  arm_id_ = get_node()->get_parameter("arm_id").as_string();
  is_gripper_loaded_ = get_node()->get_parameter("load_gripper").as_bool();

  auto k_gains = get_node()->get_parameter("k_gains").as_double_array();
  auto d_gains = get_node()->get_parameter("d_gains").as_double_array();
  if (k_gains.empty()) {
    RCLCPP_FATAL(get_node()->get_logger(), "k_gains parameter not set");
    return false;
  }
  if (k_gains.size() != static_cast<uint>(num_joints_)) {
    RCLCPP_FATAL(get_node()->get_logger(), "k_gains should be of size %d but is of size %ld",
                 num_joints_, k_gains.size());
    return false;
  }
  if (d_gains.empty()) {
    RCLCPP_FATAL(get_node()->get_logger(), "d_gains parameter not set");
    return false;
  }
  if (d_gains.size() != static_cast<uint>(num_joints_)) {
    RCLCPP_FATAL(get_node()->get_logger(), "d_gains should be of size %d but is of size %ld",
                 num_joints_, d_gains.size());
    return false;
  }
  for (int i = 0; i < num_joints_; ++i) {
    d_gains_(i) = d_gains.at(i);
    k_gains_(i) = k_gains.at(i);
  }
  return true;
}

CallbackReturn JointImpedanceIKController::on_configure(
    const rclcpp_lifecycle::State& /*previous_state*/) {
  if (!assign_parameters()) {
    return CallbackReturn::FAILURE;
  }

  franka_robot_model_ = std::make_unique<franka_semantic_components::FrankaRobotModel>(
      franka_semantic_components::FrankaRobotModel(arm_id_ + "/" + k_robot_model_interface_name,
                                                   arm_id_ + "/" + k_robot_state_interface_name));

  auto collision_client = get_node()->create_client<franka_msgs::srv::SetFullCollisionBehavior>(
      "/service_server/set_full_collision_behavior");
  compute_ik_client_ = get_node()->create_client<moveit_msgs::srv::GetPositionIK>("/compute_ik");

  while (!compute_ik_client_->wait_for_service(1s) || !collision_client->wait_for_service(1s)) {
    if (!rclcpp::ok()) {
      RCLCPP_ERROR(get_node()->get_logger(), "Interrupted while waiting for the service. Exiting.");
      return CallbackReturn::ERROR;
    }
    RCLCPP_INFO(get_node()->get_logger(), "service not available, waiting again...");
  }

  auto request = DefaultRobotBehavior::getDefaultCollisionBehaviorRequest();
  auto future_result = collision_client->async_send_request(request);

  auto success = future_result.get();

  if (!success->success) {
    RCLCPP_FATAL(get_node()->get_logger(), "Failed to set default collision behavior.");
    return CallbackReturn::ERROR;
  } else {
    RCLCPP_INFO(get_node()->get_logger(), "Default collision behavior set.");
  }

  auto parameters_client =
      std::make_shared<rclcpp::AsyncParametersClient>(get_node(), "/robot_state_publisher");
  parameters_client->wait_for_service();

  auto future = parameters_client->get_parameters({"robot_description"});
  auto result = future.get();
  if (!result.empty()) {
    robot_description_ = result[0].value_to_string();
  } else {
    RCLCPP_ERROR(get_node()->get_logger(), "Failed to get robot_description parameter.");
  }

  arm_id_ = robot_utils::getRobotNameFromDescription(robot_description_, get_node()->get_logger());

  spacemouse_sub_ = get_node()->create_subscription<geometry_msgs::msg::Twist>(
      "franka_controller/target_cartesian_velocity_percent", 10,
      [this](const geometry_msgs::msg::Twist::SharedPtr msg) { this->spacemouse_callback(msg); });

  RCLCPP_INFO(get_node()->get_logger(),
              "Subscribed to franka_controller/target_cartesian_velocity_percent.");

  return CallbackReturn::SUCCESS;
}

CallbackReturn JointImpedanceIKController::on_activate(
    const rclcpp_lifecycle::State& /*previous_state*/) {
  dq_filtered_.setZero();
  joint_positions_desired_.reserve(num_joints_);
  joint_positions_current_.reserve(num_joints_);
  joint_velocities_current_.reserve(num_joints_);
  joint_efforts_current_.reserve(num_joints_);

  franka_cartesian_pose_->assign_loaned_state_interfaces(state_interfaces_);
  franka_robot_model_->assign_loaned_state_interfaces(state_interfaces_);

  return CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn JointImpedanceIKController::on_deactivate(
    const rclcpp_lifecycle::State& /*previous_state*/) {
  franka_cartesian_pose_->release_interfaces();
  return CallbackReturn::SUCCESS;
}

void JointImpedanceIKController::spacemouse_callback(
    const geometry_msgs::msg::Twist::SharedPtr msg) {
  // The values for max_linear_pos_update and max_angular_pos_update are empirically determined.
  // These values represent a tradeoff between precision in teleop control and speed.
  // Lowering these values will make the robot move slower and more precise, while increasing them
  // will make it move faster but less precise.
  const double max_linear_pos_update = 0.007;
  const double max_angular_pos_update = 0.03;
  desired_linear_position_update_ =
      max_linear_pos_update * Eigen::Vector3d(msg->linear.x, msg->linear.y, msg->linear.z);
  desired_angular_position_update_ =
      max_angular_pos_update * Eigen::Vector3d(msg->angular.x, msg->angular.y, msg->angular.z);
  Eigen::AngleAxisd rollAngle(desired_angular_position_update_.x(), Eigen::Vector3d::UnitX());
  Eigen::AngleAxisd pitchAngle(desired_angular_position_update_.y(), Eigen::Vector3d::UnitY());
  Eigen::AngleAxisd yawAngle(desired_angular_position_update_.z(), Eigen::Vector3d::UnitZ());
  desired_angular_position_update_quaternion_ = yawAngle * pitchAngle * rollAngle;
}

}  // namespace franka_single_arm_controllers
#include "pluginlib/class_list_macros.hpp"
// NOLINTNEXTLINE
PLUGINLIB_EXPORT_CLASS(franka_single_arm_controllers::JointImpedanceIKController,
                       controller_interface::ControllerInterface)