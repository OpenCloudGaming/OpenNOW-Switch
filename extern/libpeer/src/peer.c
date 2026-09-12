#include <srtp2/srtp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "peer.h"
#include "sctp.h"
#include "utils.h"

static pthread_mutex_t peer_runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned peer_runtime_users;

int peer_init() {
  pthread_mutex_lock(&peer_runtime_mutex);
  if (peer_runtime_users == 0 && srtp_init() != srtp_err_status_ok) {
    pthread_mutex_unlock(&peer_runtime_mutex);
    LOGE("libsrtp init failed");
    return -1;
  }
  sctp_usrsctp_init();
  peer_runtime_users++;
  pthread_mutex_unlock(&peer_runtime_mutex);
  return 0;
}

void peer_deinit() {
  pthread_mutex_lock(&peer_runtime_mutex);
  if (peer_runtime_users == 0) {
    pthread_mutex_unlock(&peer_runtime_mutex);
    return;
  }
  sctp_usrsctp_deinit();
  if (--peer_runtime_users == 0)
    srtp_shutdown();
  pthread_mutex_unlock(&peer_runtime_mutex);
}
