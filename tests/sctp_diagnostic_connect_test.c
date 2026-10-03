#define main existing_sctp_reliability_main
#include "sctp_reliability_test.c"
#undef main

static unsigned diagnostics;

static void failing_log_sink(const char* message) {
  assert(message && *message);
  diagnostics++;
  errno = ENOENT;
}

int main(void) {
  owner = pthread_self();
  sctp_usrsctp_init();
  sctp_set_diagnostic_callback(failing_log_sink);
  sctp_set_diagnostics_enabled(1);
  create_pair();
  assert(opened == 2);
  assert(diagnostics > 0);
  assert(send_message(0, 0, 42, 32) == 32);
  pump();
  assert(delivered == 1 && deliveries[0].value == 42);
  destroy_pair();
  sctp_usrsctp_deinit();
  puts("SCTP connects and transfers with a failing diagnostic sink: PASS");
  return 0;
}
