#include "diagnostics/symbolizer.h"

#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <utility>

namespace monitor::diagnostics {
namespace {

std::string FormatAddress(std::uint64_t address) {
  std::ostringstream output;
  output << "0x" << std::hex << address;
  return output.str();
}

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
