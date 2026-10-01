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
static int next_handshake_result;
static int next_association_result;
static int association_creates;
static int state_changes;
static int callback_opens;
static int callback_closes;
static int callback_messages;
static int open_during_create;
static int close_during_create;
static int close_during_tick;
static PeerConnectionState last_state;

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
void sctp_tick(Sctp* sctp) {
  if (close_during_tick) {
    close_during_tick = 0;
    sctp->connected = 0;
    sctp->onclose(sctp->userdata);
  }
}
void sctp_onopen(Sctp* sctp, void (*callback)(void*)) { sctp->onopen = callback; }
void sctp_onclose(Sctp* sctp, void (*callback)(void*)) { sctp->onclose = callback; }
void sctp_onmessage(Sctp* sctp, void (*callback)(char*, size_t, void*, uint16_t)) {
  sctp->onmessage = callback;
}
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
  (void)dtls; (void)address; return next_handshake_result;
}
void dtls_srtp_reset_session(DtlsSrtp* dtls) { (void)dtls; abort(); }
int sctp_create_association(Sctp* sctp, DtlsSrtp* dtls) {
  (void)dtls;
  ++association_creates;
  if (open_during_create)
    sctp->onopen(sctp->userdata);
  if (close_during_create)
    sctp->onclose(sctp->userdata);
  return next_association_result;
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

static void state_changed(PeerConnectionState state, void* userdata) {
  assert(userdata == &association_creates);
  ++state_changes;
  last_state = state;
}

static void association_opened(void* userdata) {
  assert(userdata == &association_creates);
  ++callback_opens;
}

static void association_closed(void* userdata) {
  assert(userdata == &association_creates);
  ++callback_closes;
}

static void association_message(char* message, size_t len, void* userdata, uint16_t sid) {
  assert(userdata == &association_creates);
  assert(len == 4 && memcmp(message, "test", len) == 0 && sid == 7);
  ++callback_messages;
}

static void test_association_startup(void) {
  for (int datachannel = DATA_CHANNEL_NONE; datachannel <= DATA_CHANNEL_BINARY; ++datachannel) {
    for (int result = -1; result <= 1; ++result) {
      PeerConnection* peer = calloc(1, sizeof(*peer));
      assert(peer);
      peer->state = PEER_CONNECTION_CONNECTED;
      peer->config.datachannel = datachannel;
      peer->config.user_data = &association_creates;
      peer->oniceconnectionstatechange = state_changed;
      peer_connection_ondatachannel(peer, NULL, association_opened, association_closed);
      association_creates = state_changes = callback_opens = 0;
      open_during_create = 0;
      next_handshake_result = MBEDTLS_ERR_SSL_WANT_READ;
      assert(peer_connection_loop(peer) == 0);
      assert(peer->state == PEER_CONNECTION_CONNECTED);
      assert(association_creates == 0 && state_changes == 0);

      next_handshake_result = 0;
      next_association_result = result;
      assert(peer_connection_loop(peer) == 0);
      const PeerConnectionState expected = datachannel && result != 0
          ? PEER_CONNECTION_FAILED : PEER_CONNECTION_COMPLETED;
      assert(peer->state == expected);
      assert(state_changes == 1 && last_state == expected);
      assert(association_creates == (datachannel ? 1 : 0));
      assert(peer->sctp_create_attempted == (datachannel ? 1 : 0));
      for (int attempt = 0; attempt < 100; ++attempt)
        assert(peer_connection_loop(peer) == 0);
      assert(peer->state == expected && state_changes == 1);
      assert(association_creates == (datachannel ? 1 : 0));
      free(peer);
    }
  }
  PeerConnection* peer = calloc(1, sizeof(*peer));
  assert(peer);
  peer->state = PEER_CONNECTION_CONNECTED;
  peer->config.datachannel = DATA_CHANNEL_STRING;
  peer->config.user_data = &association_creates;
  peer_connection_ondatachannel(peer, association_message, association_opened, association_closed);
  open_during_create = 1;
  next_association_result = 0;
  assert(peer_connection_loop(peer) == 0);
  assert(peer->state == PEER_CONNECTION_COMPLETED && callback_opens == 1);
  char message[] = "test";
  peer->sctp.onmessage(message, 4, peer->sctp.userdata, 7);
  assert(callback_messages == 1);
  free(peer);
  open_during_create = 0;
  socket_reads = 0;
}

static void test_association_close(void) {
  for (int datachannel = 1; datachannel >= 0; --datachannel) {
    for (int opened = 0; opened <= 1; ++opened) {
      PeerConnection* peer = calloc(1, sizeof(*peer));
      assert(peer);
      peer->state = PEER_CONNECTION_CONNECTED;
      peer->config.datachannel = datachannel;
      peer->config.user_data = &association_creates;
      peer->oniceconnectionstatechange = state_changed;
      peer_connection_ondatachannel(peer, NULL, association_opened, association_closed);
      state_changes = callback_opens = callback_closes = 0;
      assert(peer_connection_loop(peer) == 0);
      assert(peer->state == PEER_CONNECTION_COMPLETED && state_changes == 1);
      for (int attempt = 0; attempt < 100; ++attempt)
        assert(peer_connection_loop(peer) == 0);
      assert(peer->state == PEER_CONNECTION_COMPLETED && callback_closes == 0);
      if (opened) {
        peer->sctp.connected = 1;
        peer->sctp.onopen(peer->sctp.userdata);
        assert(callback_opens == 1);
      }
      close_during_tick = 1;
      const int reads_before_close = socket_reads;
      assert(peer_connection_loop(peer) == 0);
      assert(callback_closes == 1);
      const PeerConnectionState expected = datachannel
          ? PEER_CONNECTION_FAILED : PEER_CONNECTION_COMPLETED;
      assert(peer->state == expected);
      assert(state_changes == (datachannel ? 2 : 1));
      if (datachannel)
        assert(socket_reads == reads_before_close);
      for (int attempt = 0; attempt < 100; ++attempt)
        assert(peer_connection_loop(peer) == 0);
      assert(peer->state == expected && callback_closes == 1);
      peer_connection_close(peer);
      peer->sctp.onclose(peer->sctp.userdata);
      assert(peer->state == PEER_CONNECTION_CLOSED && callback_closes == 2);
      assert(state_changes == (datachannel ? 2 : 1));
      free(peer);
    }
  }
  PeerConnection* peer = calloc(1, sizeof(*peer));
  assert(peer);
  peer->state = PEER_CONNECTION_CONNECTED;
  peer->config.datachannel = DATA_CHANNEL_STRING;
  peer->config.user_data = &association_creates;
  peer->oniceconnectionstatechange = state_changed;
  peer_connection_ondatachannel(peer, NULL, association_opened, association_closed);
  state_changes = callback_closes = 0;
  close_during_create = 1;
  assert(peer_connection_loop(peer) == 0);
  assert(peer->state == PEER_CONNECTION_FAILED && state_changes == 1 && callback_closes == 1);
  close_during_create = 0;
  free(peer);
  peer = calloc(1, sizeof(*peer));
  assert(peer);
  peer->state = PEER_CONNECTION_CONNECTED;
  peer->config.datachannel = DATA_CHANNEL_STRING;
  peer_connection_ondatachannel(peer, NULL, NULL, NULL);
  assert(peer_connection_loop(peer) == 0);
  assert(peer->state == PEER_CONNECTION_COMPLETED);
  close_during_tick = 1;
  assert(peer_connection_loop(peer) == 0);
  assert(peer->state == PEER_CONNECTION_FAILED);
  free(peer);
  socket_reads = 0;
}

int main(void) {
  test_association_startup();
  test_association_close();
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
