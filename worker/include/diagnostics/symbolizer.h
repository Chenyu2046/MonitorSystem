#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace monitor::diagnostics {

class Symbolizer {
 public:
  bool LoadKernelSymbols(const std::string& path = "/proc/kallsyms");

  std::string SymbolizeKernel(std::uint64_t address) const;
  std::string SymbolizeUser(int pid, std::uint64_t address) const;

 private:
  std::map<std::uint64_t, std::string> kernel_symbols_;
};

}  // namespace monitor::diagnostics
