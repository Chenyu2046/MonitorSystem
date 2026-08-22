/**
 * @file read_file.cpp
 * @brief 实现按行读取并拆分空格字段的辅助类。
 */

#include "utils/read_file.h"

namespace monitor {
bool ReadFile::ReadLine(std::vector<std::string>* args) {
  // 一次调用只消费一行，调用方负责决定是否继续读取。
  std::string line;
  std::getline(ifs_, line);
  if (ifs_.eof() || line.empty()) {
    return false;
  }

  // 将文本行交给字符串流，按默认空白规则拆分字段。
  std::istringstream line_ss(line);
  while (!line_ss.eof()) {  
    std::string word;
    line_ss >> word;
    args->push_back(word);
  }
  return true;
}


}  // namespace monitor
