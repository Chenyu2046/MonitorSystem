#pragma once

/**
 * @file read_file.h
 * @brief 面向空格分隔文本的轻量文件读取辅助类。
 */

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace monitor {
/** 逐行读取文件并把空格分隔字段追加到调用方容器。 */
class ReadFile {
 public:
  /** 打开指定文件；后续 ReadLine 复用该输入流。 */
  explicit ReadFile(const std::string& name) : ifs_(name) {}
  /** 关闭文件输入流。 */
  ~ReadFile() { ifs_.close(); }

  /** 读取下一行；遇到 EOF 或空行时返回 false。 */
  bool ReadLine(std::vector<std::string>* args);
  // static std::vector<std::string> GetStatsLines(const std::string& stat_file,
  //                                               const int line_count) {
  //   std::vector<std::string> stats_lines;
  //   std::ifstream buffer(stat_file);
  //   for (int line_num = 0; line_num < line_count; ++line_num) {
  //     std::string line;
  //     std::getline(buffer, line);
  //     if (line.empty()) {
  //       break;
  //     }
  //     stats_lines.push_back(line);
  //   }
  //   return stats_lines;
  // }

 private:
  std::ifstream ifs_;  // 保存实际执行文件读取的输入流。
};
}  // namespace monitor
