#define main existing_sctp_reliability_main
#include "sctp_reliability_test.c"
#undef main

static int failing_option;
static int failing_connect;
static char last_diagnostic[2048];

int __real_usrsctp_setsockopt(struct socket*, int, int, const void*, socklen_t);
int __wrap_usrsctp_setsockopt(struct socket* socket, int level, int option,
                            const void* value, socklen_t size) {
  if (option == failing_option) {
    errno = EINVAL;
    return -1;
  }
  return __real_usrsctp_setsockopt(socket, level, option, value, size);
}

int __real_usrsctp_connect(struct socket*, struct sockaddr*, socklen_t);
int __wrap_usrsctp_connect(struct socket* socket, struct sockaddr* address, socklen_t size) {
  if (failing_connect) {
    errno = ECONNREFUSED;
    return -1;
  }
  return __real_usrsctp_connect(socket, address, size);
}

static void diagnostic(const char* message) {
  if (strstr(message, "create_failed")) {
    assert(strlen(message) < sizeof(last_diagnostic));
    strcpy(last_diagnostic, message);
  }
}

int main(void) {
  owner = pthread_self();
  sctp_set_diagnostic_callback(diagnostic);
  sctp_set_diagnostics_enabled(1);
  const struct { int option; const char* stage; } cases[] = {
    {SO_LINGER, "linger"}, {SO_SNDBUF, "send_buffer"}, {SO_RCVBUF, "receive_buffer"},
    {SCTP_FRAGMENT_INTERLEAVE, "fragment_interleave"}, {SCTP_PEER_ADDR_PARAMS, "path_mtu"},
    {SCTP_RTOINFO, "retransmission_timeout"}, {SCTP_ENABLE_STREAM_RESET, "stream_reset"},
    {SCTP_NODELAY, "nodelay"}, {SCTP_EVENT, "association_events"}, {SCTP_INITMSG, "initial_streams"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    sctp_usrsctp_init();
    memset(&endpoints[0], 0, sizeof(endpoints[0]));
    last_diagnostic[0] = 0;
    failing_option = cases[i].option;
    assert(sctp_create_association(&endpoints[0], &transports[0]) == -1);
    assert(errno == EINVAL);
    char expected[128];
    snprintf(expected, sizeof(expected), "SCTP create_failed step=%s errno=%d", cases[i].stage, EINVAL);
    assert(strcmp(last_diagnostic, expected) == 0);
    assert(!endpoints[0].sock && !endpoints[0].transport && !endpoints[0].message_buf);
    sctp_usrsctp_deinit();
  }
  sctp_usrsctp_init();
  failing_option = 0;
  failing_connect = 1;
  memset(&endpoints[0], 0, sizeof(endpoints[0]));
  assert(sctp_create_association(&endpoints[0], &transports[0]) == -1);
  assert(errno == ECONNREFUSED);
  char expected[128];
  snprintf(expected, sizeof(expected), "SCTP create_failed step=connect errno=%d", ECONNREFUSED);
  assert(strcmp(last_diagnostic, expected) == 0);
  assert(!endpoints[0].sock && !endpoints[0].transport && !endpoints[0].message_buf);
  sctp_usrsctp_deinit();
  puts("SCTP setup failure diagnostics and cleanup: PASS");
  return 0;
}
