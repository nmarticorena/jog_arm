///////////////////////////////////////////////////////////////////////////////
//      Title     : jog_arm_server.h
//      Project   : jog_arm
//      Created   : 3/9/2017
//      Author    : Brian O'Neil, Blake Anderson, Andy Zelenak
//
// BSD 3-Clause License
//
// Copyright (c) 2018, Los Alamos National Security, LLC
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////////

// Server node for arm jogging with MoveIt.

#ifndef JOG_ARM_SERVER_H
#define JOG_ARM_SERVER_H

#include <Eigen/Eigenvalues>
#include <jog_msgs/JogJoint.h>
#include <moveit/move_group_interface/move_group_interface.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit/planning_scene_interface/planning_scene_interface.h>
#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/robot_state/robot_state.h>
#include <rosparam_shortcuts/rosparam_shortcuts.h>
#include <sensor_msgs/JointState.h>
#include <sensor_msgs/Joy.h>
#include <std_msgs/Bool.h>
#include <std_msgs/Float64MultiArray.h>
#include <tf/transform_listener.h>
#include <trajectory_msgs/JointTrajectory.h>

namespace jog_arm
{
// Variables to share between threads, and their mutexes
struct jog_arm_shared
{
  geometry_msgs::TwistStamped command_deltas;
  pthread_mutex_t command_deltas_mutex;

  jog_msgs::JogJoint joint_command_deltas;
  pthread_mutex_t joint_command_deltas_mutex;

  sensor_msgs::JointState joints;
  pthread_mutex_t joints_mutex;

  double collision_velocity_scale = 1;
  pthread_mutex_t collision_velocity_scale_mutex;

  // Indicates that an incoming Cartesian command is all zero velocities
  bool zero_cartesian_cmd_flag = true;
  pthread_mutex_t zero_cartesian_cmd_flag_mutex;

  // Indicates that an incoming joint angle command is all zero velocities
  bool zero_joint_cmd_flag = true;
  pthread_mutex_t zero_joint_cmd_flag_mutex;

  // Indicates that we have not received a new command in some time
  bool command_is_stale = false;
  pthread_mutex_t command_is_stale_mutex;

  // The new trajectory which is calculated
  trajectory_msgs::JointTrajectory new_traj;
  pthread_mutex_t new_traj_mutex;

  // Timestamp of incoming commands
  ros::Time incoming_cmd_stamp = ros::Time(0.);
  pthread_mutex_t incoming_cmd_stamp_mutex;

  bool ok_to_publish = false;
  pthread_mutex_t ok_to_publish_mutex;
};

// ROS params to be read
struct jog_arm_parameters
{
  std::string move_group_name, joint_topic, cartesian_command_in_topic, command_frame, command_out_topic,
      planning_frame, warning_topic, joint_command_in_topic, command_in_type, command_out_type;
  double linear_scale, rotational_scale, joint_scale, lower_singularity_threshold, hard_stop_singularity_threshold,
      lower_collision_proximity_threshold, hard_stop_collision_proximity_threshold, low_pass_filter_coeff,
      publish_period, publish_delay, incoming_command_timeout, joint_limit_margin, collision_check_rate;
  bool gazebo, collision_check, publish_joint_positions, publish_joint_velocities, publish_joint_accelerations;
};

/**
 * Class JogROSInterface - Instantiated in main(). Handles ROS subs & pubs and
 * creates the worker threads.
 */
class JogROSInterface
{
public:
  JogROSInterface();

  // Store the parameters that were read from ROS server
  static struct jog_arm_parameters ros_parameters_;

private:
  // ROS subscriber callbacks
  void deltaCartesianCmdCB(const geometry_msgs::TwistStampedConstPtr& msg);
  void deltaJointCmdCB(const jog_msgs::JogJointConstPtr& msg);
  void jointsCB(const sensor_msgs::JointStateConstPtr& msg);

  bool readParameters(ros::NodeHandle& n);

  // Jogging calculation thread
  static void* jogCalcThread(void* thread_id);

  // Collision checking thread
  static void* CollisionCheckThread(void* thread_id);

  // Variables to share between threads
  static struct jog_arm_shared shared_variables_;

  // static robot_model_loader::RobotModelLoader *model_loader_ptr_;
  static std::unique_ptr<robot_model_loader::RobotModelLoader> model_loader_ptr_;
};

/**
 * Class LowPassFilter - Filter the joint velocities to avoid jerky motion.
 */
class LowPassFilter
{
public:
  explicit LowPassFilter(double low_pass_filter_coeff);
  double filter(double new_msrmt);
  void reset(double data);
  double filter_coeff_ = 10.;

private:
  double prev_msrmts_[3] = { 0., 0., 0. };
  double prev_filtered_msrmts_[2] = { 0., 0. };
};

LowPassFilter::LowPassFilter(const double low_pass_filter_coeff)
{
  filter_coeff_ = low_pass_filter_coeff;
}

void LowPassFilter::reset(const double data)
{
  prev_msrmts_[0] = data;
  prev_msrmts_[1] = data;
  prev_msrmts_[2] = data;

  prev_filtered_msrmts_[0] = data;
  prev_filtered_msrmts_[1] = data;
}

double LowPassFilter::filter(const double new_msrmt)
{
  // Push in the new measurement
  prev_msrmts_[2] = prev_msrmts_[1];
  prev_msrmts_[1] = prev_msrmts_[0];
  prev_msrmts_[0] = new_msrmt;

  double new_filtered_msrmt = (1 / (1 + filter_coeff_ * filter_coeff_ + 1.414 * filter_coeff_)) *
                              (prev_msrmts_[2] + 2 * prev_msrmts_[1] + prev_msrmts_[0] -
                               (filter_coeff_ * filter_coeff_ - 1.414 * filter_coeff_ + 1) * prev_filtered_msrmts_[1] -
                               (-2 * filter_coeff_ * filter_coeff_ + 2) * prev_filtered_msrmts_[0]);

  // Store the new filtered measurement
  prev_filtered_msrmts_[1] = prev_filtered_msrmts_[0];
  prev_filtered_msrmts_[0] = new_filtered_msrmt;

  return new_filtered_msrmt;
}

/**
 * Class JogCalcs - Perform the Jacobian calculations.
 */
class JogCalcs
{
public:
  JogCalcs(const jog_arm_parameters& parameters, jog_arm_shared& shared_variables,
           const std::unique_ptr<robot_model_loader::RobotModelLoader>& model_loader_ptr);

protected:
  ros::NodeHandle nh_;

  moveit::planning_interface::MoveGroupInterface move_group_;

  sensor_msgs::JointState incoming_jts_;

  bool cartesianJogCalcs(const geometry_msgs::TwistStamped& cmd, jog_arm_shared& shared_variables);

  bool jointJogCalcs(const jog_msgs::JogJoint& cmd, jog_arm_shared& shared_variables);

  // Parse the incoming joint msg for the joints of our MoveGroup
  bool updateJoints();

  Eigen::VectorXd scaleCartesianCommand(const geometry_msgs::TwistStamped& command) const;

  Eigen::VectorXd scaleJointCommand(const jog_msgs::JogJoint& command) const;

  Eigen::MatrixXd pseudoInverse(const Eigen::MatrixXd& J) const;

  // This pseudoinverse calculation is more stable near stabilities. See Golub, 1965, "Calculating the Singular Values..."
  Eigen::MatrixXd pseudoInverse(const Eigen::MatrixXd& u_matrix, const Eigen::MatrixXd& v_matrix, const Eigen::MatrixXd& s_diagonals) const;

  void enforceJointVelocityLimits(Eigen::VectorXd& calculated_joint_vel);
  bool addJointIncrements(sensor_msgs::JointState& output, const Eigen::VectorXd& increments) const;

  // Reset the data stored in low-pass filters so the trajectory won't jump when
  // jogging is resumed.
  void resetVelocityFilters();

  // Avoid a singularity or other issue.
  // Needs to be handled differently for position vs. velocity control
  void halt(trajectory_msgs::JointTrajectory& jt_traj);

  void publishWarning(bool active) const;

  bool checkIfJointsWithinBounds(trajectory_msgs::JointTrajectory_<std::allocator<void>>& new_jt_traj);

  // Possibly calculate a velocity scaling factor, due to proximity of
  // singularity and direction of motion
  double decelerateForSingularity(Eigen::MatrixXd jacobian, const Eigen::VectorXd commanded_velocity);

  // Apply velocity scaling for proximity of collisions and singularities
  bool applyVelocityScaling(jog_arm_shared& shared_variables, trajectory_msgs::JointTrajectory& new_jt_traj,
                            const Eigen::VectorXd& delta_theta, double singularity_scale);

  trajectory_msgs::JointTrajectory composeOutgoingMessage(sensor_msgs::JointState& joint_state,
                                                          const ros::Time& stamp) const;

  void lowPassFilterVelocities(const Eigen::VectorXd& joint_vel);

  void lowPassFilterPositions();

  void insertRedundantPointsIntoTrajectory(trajectory_msgs::JointTrajectory& trajectory, int count) const;

  const robot_state::JointModelGroup* joint_model_group_;

  robot_state::RobotStatePtr kinematic_state_;

  sensor_msgs::JointState jt_state_, original_jts_;
  trajectory_msgs::JointTrajectory new_traj_;

  tf::TransformListener listener_;

  std::vector<jog_arm::LowPassFilter> velocity_filters_;
  std::vector<jog_arm::LowPassFilter> position_filters_;

  ros::Publisher warning_pub_;

  jog_arm_parameters parameters_;
};

class CollisionCheckThread
{
public:
  CollisionCheckThread(const jog_arm_parameters& parameters, jog_arm_shared& shared_variables,
                       const std::unique_ptr<robot_model_loader::RobotModelLoader>& model_loader_ptr);
};

}  // namespace jog_arm

#endif  // JOG_ARM_SERVER_H