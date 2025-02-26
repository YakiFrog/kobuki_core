/**
 * @file /kobuki_core/src/driver/diff_drive.cpp
 *
 * @brief Differential drive abstraction (brought in from ycs).
 *
 * License: BSD
 *   https://raw.githubusercontent.com/kobuki-base/kobuki_core/license/LICENSE
 **/

/*****************************************************************************
** Includes
*****************************************************************************/

#include "../../include/kobuki_core/modules/diff_drive.hpp"

/*****************************************************************************
** Namespaces
*****************************************************************************/

namespace kobuki {

/*****************************************************************************
** Implementation
*****************************************************************************/
DiffDrive::DiffDrive() :
  last_velocity_left(0.0),
  last_velocity_right(0.0),
  last_tick_left(0),
  last_tick_right(0),
  last_rad_left(0.0),
  last_rad_right(0.0),
  point_velocity(2,0.0), // 速度指令 [m/s, rad/s]
  radius(0.0), speed(0.0), // 速度指令 [mm/s, mm]
  bias(0.485), // トレッド幅[m]
  wheel_radius(0.205), // 車輪半径[m]
  tick_to_rad(0.00071674029f), // エンコーダ値からラジアンに変換するための係数
  diff_drive_kinematics(bias, wheel_radius)
{}

/**
 * @brief Updates the odometry from firmware stamps and encoders.
 *
 * Really horrible - could do with an overhaul.
 *
 * @param time_stamp
 * @param left_encoder
 * @param right_encoder
 * @param pose_update
 * @param pose_update_rates
 */
void DiffDrive::update(const uint16_t &time_stamp,
                       const uint16_t &left_encoder,
                       const uint16_t &right_encoder,
                       ecl::linear_algebra::Vector3d &pose_update,
                       ecl::linear_algebra::Vector3d &pose_update_rates) {
  state_mutex.lock();
  static bool init_l = false;
  static bool init_r = false;
  double left_diff_ticks = 0.0f;
  double right_diff_ticks = 0.0f;
  unsigned short curr_tick_left = 0;
  unsigned short curr_tick_right = 0;
  unsigned short curr_timestamp = 0;
  curr_timestamp = time_stamp;
  curr_tick_left = left_encoder;
  if (!init_l)
  {
    last_tick_left = curr_tick_left;
    init_l = true;
  }
  left_diff_ticks = (double)(short)((curr_tick_left - last_tick_left) & 0xffff);
  last_tick_left = curr_tick_left;
  last_rad_left += tick_to_rad * left_diff_ticks;

  curr_tick_right = right_encoder;
  if (!init_r)
  {
    last_tick_right = curr_tick_right;
    init_r = true;
  }
  right_diff_ticks = (double)(short)((curr_tick_right - last_tick_right) & 0xffff);
  last_tick_right = curr_tick_right;
  last_rad_right += tick_to_rad * right_diff_ticks;

  // TODO this line and the last statements are really ugly; refactor, put in another place
  pose_update = diff_drive_kinematics.poseUpdateFromWheelDifferential(
    tick_to_rad * left_diff_ticks,
    tick_to_rad * right_diff_ticks
  );

  if (curr_timestamp != last_timestamp)
  {
    last_diff_time = ((double)(short)((curr_timestamp - last_timestamp) & 0xffff)) / 1000.0f;
    last_timestamp = curr_timestamp;
    last_velocity_left = (tick_to_rad * left_diff_ticks) / last_diff_time;
    last_velocity_right = (tick_to_rad * right_diff_ticks) / last_diff_time;
  } else {
    // we need to set the last_velocity_xxx to zero?
  }

  pose_update_rates << pose_update[0]/last_diff_time,  // x (m)
                       pose_update[1]/last_diff_time,  // y (m)
                       pose_update[2]/last_diff_time;  // heading (rads)
  state_mutex.unlock();
}

void DiffDrive::reset() {
  state_mutex.lock();
  last_rad_left = 0.0;
  last_rad_right = 0.0;
  last_velocity_left = 0.0;
  last_velocity_right = 0.0;
  state_mutex.unlock();
}

void DiffDrive::getWheelJointStates(double &wheel_left_angle, double &wheel_left_angle_rate,
                                    double &wheel_right_angle, double &wheel_right_angle_rate) {
  state_mutex.lock();
  wheel_left_angle = last_rad_left;
  wheel_right_angle = last_rad_right;
  wheel_left_angle_rate = last_velocity_left;
  wheel_right_angle_rate = last_velocity_right;
  state_mutex.unlock();
}

// KobukiRos::subscribeVelocityCommandで呼ばれる
void DiffDrive::setVelocityCommands(const double &vx, const double &wz) {
  // vx: in m/s
  // wz: in rad/s
  std::vector<double> cmd_vel;
  cmd_vel.push_back(vx); // m/s
  cmd_vel.push_back(wz); // rad/s
  point_velocity = cmd_vel;
}

// 送信データの計算をしている場所
void DiffDrive::velocityCommands(const double &vx, const double &wz) {
  // vx: in m/s
  // wz: in rad/s

  // vxをいじれるようにする
  double vx_ = vx;

  velocity_mutex.lock();
  const double epsilon = 0.0001;

  // 追加：vxが0.1m/s以下の場合に角速度が加わると制御量がクソ大きくなってガタガタなるので，制限をかける
  // また，0.1m/s以下の場合に動かすことはない.
  if( std::abs(vx_) < 0.1 ) {
    vx_ = 0.0;
  }

  // 特別なケース1: 直進
  if( std::abs(wz) < epsilon ) {
    radius = 0.0f;
    speed  = 1000.0f * vx_;
    velocity_mutex.unlock();
    std::cout << "speed: " << speed << ", radius: " << radius << std::endl;
    return;
  }

  // 回転半径[mm]の計算
  radius = vx_ * 1000.0f / wz;

  // 特別なケース2: 超信地回転か，半径が1.0mm以下
  if( std::abs(vx_) < epsilon || std::abs(radius) <= 1.0f ) {
    speed  = 1000.0f * bias * wz / 2.0f; 
    if (std::abs(speed) < 50.0f) // 最低でも50mm/sは動かす(汚い書き方) 24/11/22
    {
      if (speed > 0.0f) 
      {
        speed = 50.0f;
      } 
      else 
      {
        speed = -50.0f;
      }
    }
    radius = 1.0f;
    velocity_mutex.unlock();
    std::cout << "speed: " << speed << ", radius: " << radius << std::endl;
    return;
  }

  // 通常の場合（移動 + 曲がる場合）
  if( radius > 0.0f ) {
    speed  = (radius + 1000.0f * bias / 2.0f) * wz;
  } else {
    speed  = (radius - 1000.0f * bias / 2.0f) * wz;
  }

  std::cout << "speed: " << speed << ", radius: " << radius << std::endl;

  velocity_mutex.unlock();
  return;
}

void DiffDrive::velocityCommands(const short &cmd_speed, const short &cmd_radius) {
  velocity_mutex.lock();
  speed = static_cast<double>(cmd_speed);   // In [mm/s]
  radius = static_cast<double>(cmd_radius); // In [mm]
  velocity_mutex.unlock();
  return;
}

std::vector<short> DiffDrive::velocityCommands() {
  velocity_mutex.lock();
  std::vector<short> cmd(2);
  cmd[0] = bound(speed);  // In [mm/s]
  cmd[1] = bound(radius); // In [mm]
  velocity_mutex.unlock();
  return cmd;
}

std::vector<double> DiffDrive::pointVelocity() const {
  return point_velocity;
}

short DiffDrive::bound(const double &value) {
  if (value > static_cast<double>(SHRT_MAX)) return SHRT_MAX;
  if (value < static_cast<double>(SHRT_MIN)) return SHRT_MIN;
  return static_cast<short>(value);
}

} // namespace kobuki
