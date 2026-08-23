#pragma once

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>

namespace monitor {

bool EnsureOrdinarySchemaReady(MYSQL* connection);

}  // namespace monitor
#endif
