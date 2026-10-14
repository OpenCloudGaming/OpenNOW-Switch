#include <assert.h>

#include "../extern/libpeer/src/agent.c"

static uint32_t now_ms;
static int send_calls;
static int send_result;
static uint8_t sent[1400];
static int sent_size;

uint32_t ports_get_epoch_time(void) {
  return now_ms;
}

int udp_socket_sendto(UdpSocket* socket, Address* addr, const uint8_t* buf, int len) {
  assert(socket->fd == 3);
  assert(addr->port == 5004);
  assert(len <= (int)sizeof(sent));
  send_calls++;
  memcpy(sent, buf, len);
  sent_size = len;
  return send_result < 0 ? send_result : len;
}

static StunMessage parse_sent(void) {
  StunMessage msg;
  memset(&msg, 0, sizeof(msg));
  memcpy(msg.buf, sent, sent_size);
  msg.size = sent_size;
  stun_parse_msg_buf(&msg);
  return msg;
}

static int has_attribute(uint16_t wanted) {
  size_t offset = sizeof(StunHeader);
  while (offset + sizeof(StunAttribute) <= (size_t)sent_size) {
    StunAttribute* attr = (StunAttribute*)(sent + offset);
    if (ntohs(attr->type) == wanted)
      return 1;
    offset += sizeof(StunAttribute) + ((ntohs(attr->length) + 3) & ~3u);
  }
  return 0;
}

static void respond_to_last_request(Agent* agent) {
  StunMessage request = parse_sent();
  StunMessage response;
  memset(&response, 0, sizeof(response));
  stun_msg_create(&response, STUN_CLASS_RESPONSE | STUN_METHOD_BINDING);
  memcpy(((StunHeader*)response.buf)->transaction_id,
         ((StunHeader*)request.buf)->transaction_id,
         sizeof(((StunHeader*)request.buf)->transaction_id));
  stun_msg_finish(&response, STUN_CREDENTIAL_SHORT_TERM, agent->remote_upwd,
                  strlen(agent->remote_upwd));
  stun_parse_msg_buf(&response);
  agent_process_stun_response(agent, &response);
}

int main(void) {
  Agent agent = {0};
  IceCandidate remote = {0};
  IceCandidatePair pair = {0};
  uint32_t first_transaction[3];

  agent.udp_sockets[0].fd = 3;
  agent.udp_sockets[1].fd = -1;
  agent.mode = AGENT_MODE_CONTROLLING;
  strcpy(agent.local_ufrag, "loc1");
  strcpy(agent.local_upwd, "local-password-0123456789");
  strcpy(agent.remote_ufrag, "2efecf37");
  strcpy(agent.remote_upwd, "26b335b899a84ffab9aaf38ddad1e2b4");
  remote.addr.family = AF_INET;
  remote.addr.port = 5004;
  pair.remote = &remote;
  pair.priority = 0x6e7f1eff;
  pair.state = ICE_CANDIDATE_STATE_SUCCEEDED;

  assert(agent_maybe_send_consent_request(&agent, 0) == 0);
  assert(send_calls == 0);

  agent.nominated_pair = &pair;
  agent.selected_pair = &pair;
  now_ms = 1000;
  assert(agent_maybe_send_consent_request(&agent, now_ms) == 1);
  assert(send_calls == 1);
  StunMessage first = parse_sent();
  assert(first.stunclass == STUN_CLASS_REQUEST);
  assert(first.stunmethod == STUN_METHOD_BINDING);
  assert(stun_msg_is_valid(sent, sent_size, agent.remote_upwd) == 0);
  assert(has_attribute(STUN_ATTR_TYPE_ICE_CONTROLLING));
  assert(has_attribute(STUN_ATTR_TYPE_PRIORITY));
  assert(!has_attribute(STUN_ATTR_TYPE_USE_CANDIDATE));
  memcpy(first_transaction, ((StunHeader*)sent)->transaction_id, sizeof(first_transaction));

  now_ms = 1000 + CONFIG_ICE_CONSENT_INTERVAL_MS - 1;
  assert(agent_maybe_send_consent_request(&agent, now_ms) == 0);
  assert(send_calls == 1);

  now_ms = 1040;
  respond_to_last_request(&agent);
  assert(agent.binding_responses_received == 1);
  assert(agent.smoothed_binding_rtt_ms == 40);
  assert(agent.binding_request_time == 1040);

  now_ms = 1000 + CONFIG_ICE_CONSENT_INTERVAL_MS;
  assert(agent_maybe_send_consent_request(&agent, now_ms) == 1);
  assert(send_calls == 2);
  assert(stun_msg_is_valid(sent, sent_size, agent.remote_upwd) == 0);
  assert(memcmp(first_transaction, ((StunHeader*)sent)->transaction_id,
                sizeof(first_transaction)) != 0);

  send_result = -1;
  now_ms += CONFIG_ICE_CONSENT_INTERVAL_MS;
  assert(agent_maybe_send_consent_request(&agent, now_ms) == -1);
  assert(agent_maybe_send_consent_request(&agent, now_ms + 1) == 0);
  assert(send_calls == 3);
  assert(agent.consent_attempts == 3 && agent.consent_requests_sent == 2);

  send_result = 0;
  now_ms += CONFIG_ICE_CONSENT_INTERVAL_MS;
  assert(agent_maybe_send_consent_request(&agent, now_ms) == 1);
  assert(agent.consent_requests_sent == 3);

  agent.mode = AGENT_MODE_CONTROLLED;
  now_ms += CONFIG_ICE_CONSENT_INTERVAL_MS;
  assert(agent_maybe_send_consent_request(&agent, now_ms) == 1);
  assert(has_attribute(STUN_ATTR_TYPE_ICE_CONTROLLED));
  assert(!has_attribute(STUN_ATTR_TYPE_USE_CANDIDATE));

  agent_clear_candidates(&agent);
  assert(agent.consent_attempts == 0 && agent.consent_requests_sent == 0);
  assert(agent.binding_responses_received == 0);
  assert(agent_maybe_send_consent_request(&agent, now_ms) == 0);
  return 0;
}
