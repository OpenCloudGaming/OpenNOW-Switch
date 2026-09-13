#include <assert.h>

#include "../extern/libpeer/src/peer_connection.c"

static int socket_reads;
static int socket_result;

int agent_recv(Agent* agent, uint8_t* buffer, int length) {
  (void)agent;
  (void)buffer;
  (void)length;
  ++socket_reads;
  return socket_result;
}

int agent_recv_nonblocking(Agent* agent, uint8_t* buffer, int length) {
  return agent_recv(agent, buffer, length);
}

int main(void) {
  PeerConnection* peer = calloc(1, sizeof(*peer));
  assert(peer);
  peer->dtls_srtp.user_data = peer;
  peer->state = PEER_CONNECTION_COMPLETED;
  peer->agent_ret = 5;
  memcpy(peer->agent_buf, "\x17\x01\x02\x03\x04", 5);
  unsigned char buffer[32];
  assert(peer_connection_dtls_srtp_recv(&peer->dtls_srtp, buffer, sizeof(buffer)) == 5);
  assert(memcmp(buffer, "\x17\x01\x02\x03\x04", 5) == 0);
  assert(peer_connection_dtls_srtp_recv(&peer->dtls_srtp, buffer, sizeof(buffer)) ==
         MBEDTLS_ERR_SSL_WANT_READ);
  assert(socket_reads == 0);

  socket_result = 12;
  assert(peer_connection_dtls_srtp_recv(&peer->dtls_srtp, buffer, sizeof(buffer)) ==
         MBEDTLS_ERR_SSL_WANT_READ);
  assert(socket_reads == 0);

  peer->agent_ret = 5;
  assert(peer_connection_dtls_srtp_recv(&peer->dtls_srtp, buffer, 4) < 0);
  assert(peer->agent_ret <= 0);
  assert(socket_reads == 0);

  peer->state = PEER_CONNECTION_CONNECTED;
  socket_result = 0;
  assert(peer_connection_dtls_srtp_recv(&peer->dtls_srtp, buffer, sizeof(buffer)) ==
         MBEDTLS_ERR_SSL_WANT_READ);
  assert(socket_reads == 1);
  socket_result = 12;
  assert(peer_connection_dtls_srtp_recv(&peer->dtls_srtp, buffer, sizeof(buffer)) == 12);
  assert(socket_reads == 2);
  free(peer);
}
