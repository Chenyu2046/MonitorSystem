#pragma once

/**
 * @file symbolizer.h
 * @brief 将内核和用户态 stack 地址转换为可读符号的接口。
 *
 * 内核符号来自 /proc/kallsyms，用户态模块范围来自目标进程的
 * /proc/<pid>/maps；符号化失败时返回地址字符串，不能保证得到函数名。
 */

#include <cstdint>
#include <map>
#include <string>

namespace monitor::diagnostics {

/**
 * @brief 为 Profiling 诊断快照提供尽力而为的地址符号化。
 */
class Symbolizer {
 public:
  /** @brief 加载并替换内核地址到符号名的有序索引。 */
  bool LoadKernelSymbols(const std::string& path = "/proc/kallsyms");

  /** @brief 将内核地址映射为 symbol+offset 或原始地址。 */
  std::string SymbolizeKernel(std::uint64_t address) const;
  /** @brief 根据进程 maps 将用户地址映射为模块+offset 或原始地址。 */
  std::string SymbolizeUser(int pid, std::uint64_t address) const;

 private:
  std::map<std::uint64_t, std::string> kernel_symbols_;
};

}  // namespace monitor::diagnostics
