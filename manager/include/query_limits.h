#pragma once

#include <algorithm>
#include <cstdint>

namespace monitor {

constexpr int kMaxPageSize = 1000;
constexpr int64_t kMaxQueryOffset = 100000;

// OFFSET 分页会扫描并跳过前序行。服务层和数据层必须复用同一归一化规则，
// 使客户端回显的页号、SQL OFFSET 和单次数据库工作集保持一致。
inline void NormalizeQueryPagination(int* page, int* page_size) {
  if (*page < 1) *page = 1;
  if (*page_size < 1) *page_size = 100;
  *page_size = std::min(*page_size, kMaxPageSize);
  const int max_page =
      static_cast<int>(kMaxQueryOffset / *page_size) + 1;
  *page = std::min(*page, max_page);
}

}  // namespace monitor
