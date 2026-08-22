#pragma once

/**
 * @file utils.h
 * @brief Worker 通用时间换算辅助函数。
 */

#include <boost/chrono.hpp>

namespace monitor {
/** 提供无需实例化即可调用的稳态时钟换算工具。 */
class Utils {
 public:
  /** 返回 t1 - t2 的秒数，适用于不受系统校时影响的 steady_clock。 */
  static double SteadyTimeSecond(
      const boost::chrono::steady_clock::time_point &t1,
      const boost::chrono::steady_clock::time_point &t2) {
    boost::chrono::duration<double> time_second = t1 - t2;
    return time_second.count();
  }
};
}  // namespace monitor
