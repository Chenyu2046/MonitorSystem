/**
 * @file symbolizer.cpp
 * @brief 基于 /proc/kallsyms 和 /proc/<pid>/maps 的尽力符号化实现。
 *
 * Profiling 数据链路中的 stack id 先由 BPF map 还原为地址，再在这里
 * 转成可读的 kernel symbol 或用户模块偏移；符号化失败只降低可读性，
 * 不阻止诊断快照继续上报。
 */

#include "diagnostics/symbolizer.h"

#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <utility>

namespace monitor::diagnostics {
namespace {

/** @brief 将无法符号化的地址格式化为十六进制文本。 */
std::string FormatAddress(std::uint64_t address) {
  std::ostringstream output;
  output << "0x" << std::hex << address;
  return output.str();
}

/** @brief 格式化用户模块路径和相对偏移。 */
std::string FormatModuleOffset(const std::string& module,
                               std::uint64_t offset) {
  std::ostringstream output;
  if (!module.empty()) {
    output << module;
  } else {
    output << "[unknown]";
  }
  output << "+0x" << std::hex << offset;
  return output.str();
}

}  // namespace

bool Symbolizer::LoadKernelSymbols(const std::string& path) {
  // 先构造局部 map，只有完整读取且非空时才替换现有索引，避免一次
  // 读取失败清空之前可用的内核符号。
  std::ifstream input(path);
  if (!input) {
    return false;
  }

  std::map<std::uint64_t, std::string> symbols;
  std::uint64_t address = 0;
  char type = '\0';
  std::string name;
  while (input >> std::hex >> address >> type >> name) {
    symbols[address] = name;
  }
  if (symbols.empty()) {
    return false;
  }
  kernel_symbols_ = std::move(symbols);
  return true;
}

std::string Symbolizer::SymbolizeKernel(std::uint64_t address) const {
  // upper_bound 后退到不大于 address 的最近符号，以 symbol+offset
  // 表示栈地址；地址落在第一个符号之前时保留原始十六进制值。
  const auto next = kernel_symbols_.upper_bound(address);
  if (next == kernel_symbols_.begin()) {
    return FormatAddress(address);
  }
  auto symbol = next;
  --symbol;
  std::ostringstream offset;
  offset << std::hex << address - symbol->first;
  return symbol->second + "+0x" + offset.str();
}

std::string Symbolizer::SymbolizeUser(int pid, std::uint64_t address) const {
  // /proc/<pid>/maps 只提供模块范围和文件偏移，不提供函数级 DWARF
  // 名称，因此当前结果语义是 module+offset 而非完整函数符号。
  std::ifstream input("/proc/" + std::to_string(pid) + "/maps");
  if (!input) {
    return FormatAddress(address);
  }

  std::string range;
  std::string permissions;
  std::uint64_t file_offset = 0;
  std::string device;
  std::uint64_t inode = 0;
  while (input >> range >> permissions >> std::hex >> file_offset >> device >>
         std::dec >> inode) {
    std::string module;
    std::getline(input, module);
    if (!module.empty() && module.front() == ' ') {
      module.erase(0, 1);
    }

    const std::size_t separator = range.find('-');
    if (separator == std::string::npos) {
      continue;
    }
    const auto start = std::stoull(range.substr(0, separator), nullptr, 16);
    const auto end = std::stoull(range.substr(separator + 1), nullptr, 16);
    if (address >= start && address < end) {
      return FormatModuleOffset(module, file_offset + address - start);
    }
  }
  return FormatAddress(address);
}

}  // namespace monitor::diagnostics
