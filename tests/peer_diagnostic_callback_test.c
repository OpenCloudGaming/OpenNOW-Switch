#include "../extern/libpeer/src/peer_connection.c"
#include "../extern/libpeer/src/sctp.c"

#include <assert.h>

static unsigned calls;
static char recorded[2048];

static void record(const char* message) {
  assert(strlen(message) < sizeof(recorded));
  strcpy(recorded, message);
  calls++;
  errno = ERANGE;
}

int main(void) {
  peer_connection_set_diagnostic_callback(record);
  peer_connection_set_diagnostics_enabled(0);
  peer_connection_diag_log("hidden");
  sctp_diag_log("hidden");
  assert(calls == 0);

  peer_connection_set_diagnostics_enabled(1);
  errno = EINPROGRESS;
  peer_connection_diag_log("sctp_create_done ret=%d", -1);
  assert(calls == 1 && strcmp(recorded, "LIBPEER sctp_create_done ret=-1") == 0);
  assert(errno == EINPROGRESS);
  errno = EINPROGRESS;
  sctp_diag_log("create_failed step=%s errno=%d", "send_buffer", 22);
  assert(calls == 2 && strcmp(recorded, "SCTP create_failed step=send_buffer errno=22") == 0);
  assert(errno == EINPROGRESS);

  char long_message[4096];
  memset(long_message, 'x', sizeof(long_message) - 1);
  long_message[sizeof(long_message) - 1] = 0;
  peer_connection_diag_log("%s", long_message);
  assert(calls == 3 && strlen(recorded) == sizeof(recorded) - 1);
  sctp_diag_log("%s", long_message);
  assert(calls == 4 && strlen(recorded) == sizeof(recorded) - 1);

  peer_connection_set_diagnostic_callback(NULL);
  peer_connection_diag_log("no sink");
  sctp_diag_log("no sink");
  assert(calls == 4);
  return 0;
}
