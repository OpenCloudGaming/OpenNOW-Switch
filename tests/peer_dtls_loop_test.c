#include <assert.h>

#include "../extern/libpeer/src/peer_connection.c"

static int socket_reads;
static int dtls_reads;
static int sctp_records;
static int rtp_records;
static int pending_records;
static int retain_pending_records;
static int next_dtls_result;
static int consume_datagram;
static uint8_t socket_packet[32];
static int socket_size;

int agent_recv_nonblocking(Agent* agent, uint8_t* buffer, int length) {
  (void)agent;
  ++socket_reads;
  assert(socket_size <= length);
  memcpy(buffer, socket_packet, socket_size);
  const int result = socket_size;
  socket_size = 0;
  return result;
}

int mbedtls_ssl_check_pending(const mbedtls_ssl_context* ssl) {
  (void)ssl;
  return pending_records != 0;
}

int dtls_srtp_read(DtlsSrtp* dtls, uint8_t* buffer, size_t length) {
  ++dtls_reads;
  if (consume_datagram) {
    unsigned char datagram[CONFIG_MTU];
    assert(peer_connection_dtls_srtp_recv(dtls, datagram, sizeof(datagram)) == 5);
    assert(datagram[0] == 0x17);
    assert(peer_connection_dtls_srtp_recv(dtls, datagram, sizeof(datagram)) ==
           MBEDTLS_ERR_SSL_WANT_READ);
  }
  if (pending_records && !retain_pending_records)
    --pending_records;
  if (next_dtls_result > 0) {
    assert((size_t)next_dtls_result <= length);
    memset(buffer, 0x55, next_dtls_result);
  }
  return next_dtls_result;
}

void sctp_incoming_data(Sctp* sctp, char* data, size_t length) {
  (void)sctp;
  assert(length == 3 && data[0] == 0x55);
  ++sctp_records;
}

int rtp_decoder_decode(RtpDecoder* decoder, const uint8_t* data, size_t length) {
  (void)decoder;
  assert(length == 14 && data[0] == 0x80);
  ++rtp_records;
  return 0;
}

int rtp_packet_validate(uint8_t* data, size_t length) {
  return length == 14 && data[0] == 0x80;
}
uint32_t rtp_get_ssrc(uint8_t* packet) { (void)packet; return 42; }
void rtp_decoder_poll(RtpDecoder* decoder) { (void)decoder; }
int dtls_srtp_probe(uint8_t* data) { return data[0] >= 0x14 && data[0] <= 0x17; }
void sctp_tick(Sctp* sctp) { (void)sctp; }
int sctp_is_connected(Sctp* sctp) { (void)sctp; return 1; }
uint32_t ports_get_epoch_time(void) { return 1; }
uint32_t ports_get_monotonic_time(void) { return 1; }
int addr_to_string(const Address* address, char* buffer, size_t size) {
  (void)address;
  if (size) buffer[0] = 0;
  return 0;
}
int agent_select_candidate_pair(Agent* agent) { (void)agent; abort(); }
int agent_connectivity_check(Agent* agent) { (void)agent; abort(); }
int agent_fail_nominated_remote(Agent* agent) { (void)agent; abort(); }
int dtls_srtp_handshake(DtlsSrtp* dtls, Address* address) {
  (void)dtls; (void)address; abort();
}
void dtls_srtp_reset_session(DtlsSrtp* dtls) { (void)dtls; abort(); }
int sctp_create_association(Sctp* sctp, DtlsSrtp* dtls) {
  (void)sctp; (void)dtls; abort();
}
int dtls_srtp_decrypt_rtp_packet(DtlsSrtp* dtls, uint8_t* packet, int* bytes) {
  (void)dtls; (void)packet; (void)bytes; return 0;
}
int dtls_srtp_decrypt_rtcp_packet(DtlsSrtp* dtls, uint8_t* packet, int* bytes) {
  (void)dtls; (void)packet; (void)bytes; return 0;
}
int dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls, uint8_t* packet, int* bytes) {
  (void)dtls; (void)packet; (void)bytes; return 0;
}
int agent_send(Agent* agent, const uint8_t* buffer, int length) {
  (void)agent; (void)buffer; return length;
}

static void queue_dtls(void) {
  memcpy(socket_packet, "\x17\x01\x02\x03\x04", 5);
  socket_size = 5;
}

int main(void) {
  PeerConnection* peer = calloc(1, sizeof(*peer));
  assert(peer);
  peer->state = PEER_CONNECTION_COMPLETED;
  peer->dtls_srtp.user_data = peer;

  queue_dtls();
  next_dtls_result = MBEDTLS_ERR_SSL_WANT_WRITE;
  assert(peer_connection_loop(peer) == 1);
  assert(socket_reads == 1 && dtls_reads == 1 && sctp_records == 0);
  assert(peer->dtls_pending && peer->agent_ret == 5);

  memcpy(socket_packet, "\x80\x60\x00\x01\x00\x00\x00\x00\x00\x00\x00\x2a\x65\x80", 14);
  socket_size = 14;
  consume_datagram = 1;
  next_dtls_result = 3;
  assert(peer_connection_loop(peer) == 1);
  assert(socket_reads == 1 && dtls_reads == 2 && sctp_records == 1);
  assert(!peer->dtls_pending && peer->agent_ret < 0);

  consume_datagram = 0;
  pending_records = 2;
  assert(peer_connection_loop(peer) == 1);
  assert(socket_reads == 1 && dtls_reads == 3 && sctp_records == 2);
  assert(peer_connection_loop(peer) == 1);
  assert(socket_reads == 1 && dtls_reads == 4 && sctp_records == 3);
  assert(peer_connection_loop(peer) == 1);
  assert(socket_reads == 2 && rtp_records == 1);
  assert(peer->completed_udp_packets == 2 && peer->completed_dtls_packets == 1);

  queue_dtls();
  consume_datagram = 1;
  next_dtls_result = MBEDTLS_ERR_SSL_WANT_READ;
  assert(peer_connection_loop(peer) == 1);
  assert(dtls_reads == 5 && !peer->dtls_pending);
  assert(peer_connection_loop(peer) == 0);
  assert(dtls_reads == 5 && socket_reads == 4);

  memcpy(socket_packet, "\x80\x60\x00\x02\x00\x00\x00\x00\x00\x00\x00\x2a\x65\x80", 14);
  socket_size = 14;
  consume_datagram = 0;
  pending_records = 1;
  retain_pending_records = 1;
  assert(peer_connection_loop(peer) == 1);
  assert(rtp_records == 2 && dtls_reads == 6 && socket_reads == 5);
  pending_records = 0;
  retain_pending_records = 0;

  queue_dtls();
  next_dtls_result = MBEDTLS_ERR_SSL_WANT_WRITE;
  assert(peer_connection_loop(peer) == 1);
  const int reads_before_retry = socket_reads;
  for (int attempt = 0; attempt < 100; ++attempt) {
    assert(peer_connection_loop(peer) == 0);
    assert(peer->dtls_pending && peer->agent_ret == 5);
    assert(socket_reads == reads_before_retry);
  }
  consume_datagram = 1;
  next_dtls_result = MBEDTLS_ERR_SSL_WANT_READ;
  assert(peer_connection_loop(peer) == 0);
  assert(!peer->dtls_pending);

  queue_dtls();
  consume_datagram = 0;
  next_dtls_result = MBEDTLS_ERR_SSL_INVALID_RECORD;
  assert(peer_connection_loop(peer) == 1);
  assert(!peer->dtls_pending && peer->agent_ret < 0);
  queue_dtls();
  next_dtls_result = MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
  assert(peer_connection_loop(peer) == 1);
  assert(peer->state == PEER_CONNECTION_CLOSED && !peer->dtls_pending);
  free(peer);
}
