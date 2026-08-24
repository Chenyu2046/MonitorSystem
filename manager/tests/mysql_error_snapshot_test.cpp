#include <cassert>

#include "mysql_error_snapshot.h"

int main() {
  unsigned int source_errno = 1064;
  const char* source_error = "original statement failure";
  const char* source_sqlstate = "42000";

  monitor::MysqlErrorSnapshot snapshot;
  snapshot.Capture(source_errno, source_error, source_sqlstate);

  // Simulate ROLLBACK changing the connection's error state.
  source_errno = 0;
  source_error = "rollback state";
  source_sqlstate = "00000";

  assert(snapshot.failed_errno == 1064);
  assert(snapshot.failed_error == "original statement failure");
  assert(snapshot.failed_sqlstate == "42000");

  monitor::MysqlErrorSnapshot no_connection;
  no_connection.Capture(0, nullptr, nullptr);
  assert(no_connection.failed_errno == 0);
  assert(no_connection.failed_error.empty());
  assert(no_connection.failed_sqlstate.empty());
  return 0;
}
