/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file pre_process_submodule.cc
 */

#include "modules/control/submodules/pre_process_submodule.h"
#include "modules/common/adapters/adapter_gflags.h"
#include "modules/common/time/time.h"
#include "modules/common/vehicle_state/vehicle_state_provider.h"
#include "modules/control/common/control_gflags.h"

namespace apollo {
namespace control {
using apollo::canbus::Chassis;
using apollo::common::ErrorCode;
using apollo::common::Status;
using apollo::common::VehicleStateProvider;
using apollo::common::time::Clock;
using apollo::localization::LocalizationEstimate;
using apollo::planning::ADCTrajectory;

PreProcessSubmodule::PreProcessSubmodule()
    : monitor_logger_buffer_(common::monitor::MonitorMessageItem::CONTROL) {}

PreProcessSubmodule::~PreProcessSubmodule() {}

std::string PreProcessSubmodule::Name() const {
  return FLAGS_pre_process_submodule_name;
}

bool PreProcessSubmodule::Init() {
  if (!cyber::common::GetProtoFromFile(FLAGS_control_common_conf_file,
                                       &control_common_conf_)) {
    AERROR << "Unable to load control common conf file: " +
                  FLAGS_control_common_conf_file;
    return false;
  }

  /*  initialize readers and writers */
  cyber::ReaderConfig chassis_reader_config;
  chassis_reader_config.channel_name = FLAGS_chassis_topic;
  chassis_reader_config.pending_queue_size = FLAGS_chassis_pending_queue_size;
  chassis_reader_ =
      node_->CreateReader<Chassis>(chassis_reader_config, nullptr);
  CHECK(chassis_reader_ != nullptr);

  cyber::ReaderConfig planning_reader_config;
  planning_reader_config.channel_name = FLAGS_planning_trajectory_topic;
  planning_reader_config.pending_queue_size = FLAGS_planning_pending_queue_size;
  trajectory_reader_ =
      node_->CreateReader<ADCTrajectory>(planning_reader_config, nullptr);
  CHECK(trajectory_reader_ != nullptr);

  cyber::ReaderConfig localization_reader_config;
  localization_reader_config.channel_name = FLAGS_localization_topic;
  localization_reader_config.pending_queue_size =
      FLAGS_localization_pending_queue_size;
  localization_reader_ = node_->CreateReader<LocalizationEstimate>(
      localization_reader_config, nullptr);
  CHECK(localization_reader_ != nullptr);

  cyber::ReaderConfig pad_msg_reader_config;
  pad_msg_reader_config.channel_name = FLAGS_pad_topic;
  pad_msg_reader_config.pending_queue_size = FLAGS_pad_msg_pending_queue_size;
  pad_msg_reader_ =
      node_->CreateReader<PadMessage>(pad_msg_reader_config, nullptr);
  CHECK(pad_msg_reader_ != nullptr);

  // set initial vehicle state by cmd
  // need to sleep, because advertised channel is not ready immediately
  // simple test shows a short delay of 80 ms or so
  AINFO << "Control resetting vehicle state, sleeping for 1000 ms ...";
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  // should init_vehicle first, let car enter work status, then use status msg
  // trigger control
  AINFO << "Control default driving action is: "
        << DrivingAction_Name(control_common_conf_.action());
  pad_msg_.set_action(control_common_conf_.action());

  return true;
}

void PreProcessSubmodule::OnChassis(const std::shared_ptr<Chassis> &chassis) {
  ADEBUG << "Received chassis data: run chassis callback.";
  std::lock_guard<std::mutex> lock(mutex_);
  latest_chassis_.CopyFrom(*chassis);
}

void PreProcessSubmodule::OnPad(const std::shared_ptr<PadMessage> &pad) {
  pad_msg_.CopyFrom(*pad);
  ADEBUG << "Received Pad Msg:" << pad_msg_.DebugString();
  AERROR_IF(!pad_msg_.has_action()) << "pad message check failed!";

  // do something according to pad message
  if (pad_msg_.action() == DrivingAction::RESET) {
    AINFO << "Control received RESET action!";
    estop_ = false;
    estop_reason_.clear();
  }
  pad_received_ = true;
}

void PreProcessSubmodule::OnPlanning(
    const std::shared_ptr<ADCTrajectory> &trajectory) {
  ADEBUG << "Received chassis data: run trajectory callback.";
  std::lock_guard<std::mutex> lock(mutex_);
  latest_trajectory_.CopyFrom(*trajectory);
}

void PreProcessSubmodule::OnLocalization(
    const std::shared_ptr<LocalizationEstimate> &localization) {
  ADEBUG << "Received control data: run localization message callback.";
  std::lock_guard<std::mutex> lock(mutex_);
  latest_localization_.CopyFrom(*localization);
}

void PreProcessSubmodule::OnMonitor(
    const common::monitor::MonitorMessage &monitor_message) {
  for (const auto &item : monitor_message.item()) {
    if (item.log_level() == common::monitor::MonitorMessageItem::FATAL) {
      estop_ = true;
      return;
    }
  }
}

Status PreProcessSubmodule::CheckInput(LocalView *local_view) {
  ADEBUG << "Received localization:"
         << local_view->localization.ShortDebugString();
  ADEBUG << "Received chassis:" << local_view->chassis.ShortDebugString();

  if (!local_view->trajectory.estop().is_estop() &&
      local_view->trajectory.trajectory_point().empty()) {
    AWARN_EVERY(100) << "planning has no trajectory point. ";
    std::string msg("planning has no trajectory point. planning_seq_num:");
    msg += std::to_string(local_view->trajectory.header().sequence_num());
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, msg);
  }

  for (auto &trajectory_point :
       *local_view->trajectory.mutable_trajectory_point()) {
    if (std::abs(trajectory_point.v()) <
            control_common_conf_.minimum_speed_resolution() &&
        std::abs(trajectory_point.a()) <
            control_common_conf_.max_acceleration_when_stopped()) {
      trajectory_point.set_v(0.0);
      trajectory_point.set_a(0.0);
    }
  }

  VehicleStateProvider::Instance()->Update(local_view->localization,
                                           local_view->chassis);

  return Status::OK();
}

Status PreProcessSubmodule::CheckTimestamp(const LocalView &local_view) {
  if (!control_common_conf_.enable_input_timestamp_check() ||
      control_common_conf_.is_control_test_mode()) {
    ADEBUG << "Skip input timestamp check by gflags.";
    return Status::OK();
  }
  double current_timestamp = Clock::NowInSeconds();
  double localization_diff =
      current_timestamp - local_view.localization.header().timestamp_sec();
  if (localization_diff > (control_common_conf_.max_localization_miss_num() *
                           control_common_conf_.localization_period())) {
    AERROR << "Localization msg lost for " << std::setprecision(6)
           << localization_diff << "s";
    monitor_logger_buffer_.ERROR("Localization msg lost");
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "Localization msg timeout");
  }

  double chassis_diff =
      current_timestamp - local_view.chassis.header().timestamp_sec();
  if (chassis_diff > (control_common_conf_.max_chassis_miss_num() *
                      control_common_conf_.chassis_period())) {
    AERROR << "Chassis msg lost for " << std::setprecision(6) << chassis_diff
           << "s";
    monitor_logger_buffer_.ERROR("Chassis msg lost");
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "Chassis msg timeout");
  }

  double trajectory_diff =
      current_timestamp - local_view.trajectory.header().timestamp_sec();
  if (trajectory_diff > (control_common_conf_.max_planning_miss_num() *
                         control_common_conf_.trajectory_period())) {
    AERROR << "Trajectory msg lost for " << std::setprecision(6)
           << trajectory_diff << "s";
    monitor_logger_buffer_.ERROR("Trajectory msg lost");
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "Trajectory msg timeout");
  }
  return Status::OK();
}

}  // namespace control
}  // namespace apollo