#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "sctp.h"
#include <usrsctp.h>

enum { QUEUE_CAPACITY = 4096, MAX_DELIVERIES = 512 };

typedef struct {
  unsigned destination;
  size_t len;
  uint8_t data[SCTP_MTU];
} Packet;

typedef struct {
  uint16_t sid;
  size_t len;
  uint8_t value;
} Delivery;

static Sctp endpoints[2];
static DtlsSrtp transports[2];
static Packet packets[QUEUE_CAPACITY];
static Delivery deliveries[MAX_DELIVERIES];
static unsigned head, tail, delivered;
static unsigned drop_data, duplicate_data, drop_sack, drop_init;
static unsigned dropped, data_packets, dcep_acks, unordered_packets, forward_tsns;
static unsigned opened, closed;
static atomic_uint_fast64_t fake_ms = 100000;
static pthread_t owner;
static pthread_t owners[2];
static int threaded;
static Sctp* threaded_endpoints[2];
static DtlsSrtp* threaded_transports[2];
static pthread_mutex_t wire_mutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t ports_get_monotonic_time(void) {
  return (uint32_t)fake_ms;
}

int __wrap_gettimeofday(struct timeval* tv, void* timezone) {
  (void)timezone;
  const uint64_t now = fake_ms;
  tv->tv_sec = (time_t)(now / 1000);
  tv->tv_usec = (suseconds_t)((now % 1000) * 1000);
  return 0;
}

static uint16_t read16(const uint8_t* data) {
  uint16_t value;
  memcpy(&value, data, sizeof(value));
  return ntohs(value);
}

static uint32_t read32(const uint8_t* data) {
  uint32_t value;
  memcpy(&value, data, sizeof(value));
  return ntohl(value);
}

int dtls_srtp_write(DtlsSrtp* transport, const uint8_t* data, size_t len) {
  DtlsSrtp* first = threaded ? threaded_transports[0] : &transports[0];
  DtlsSrtp* second = threaded ? threaded_transports[1] : &transports[1];
  assert(transport == first || transport == second);
  const unsigned source = transport == first ? 0 : 1;
  assert(pthread_equal(threaded ? owners[source] : owner, pthread_self()));
  pthread_mutex_lock(&wire_mutex);
  assert(len >= 16 && len <= SCTP_MTU);
  int has_data = 0, has_sack = 0, has_init = 0;
  for (size_t pos = 12; pos + 4 <= len;) {
    const size_t chunk_len = read16(data + pos + 2);
    assert(chunk_len >= 4 && chunk_len <= len - pos);
    if (data[pos] == SCTP_DATA) {
      assert(chunk_len >= 16);
      has_data = 1;
      data_packets++;
      if (data[pos + 1] & 4)
        unordered_packets++;
      if (read32(data + pos + 12) == PPID_CONTROL &&
          chunk_len >= 17 && data[pos + 16] == DATA_CHANNEL_ACK)
        dcep_acks++;
    }
    has_sack |= data[pos] == SCTP_SELECTIVE_ACK;
    has_init |= data[pos] == SCTP_INITIATION;
    forward_tsns += data[pos] == SCTP_FORWARD_CUM_TSN;
    pos += (chunk_len + 3) & ~(size_t)3;
  }
  if ((has_data && drop_data) || (has_sack && drop_sack) || (has_init && drop_init)) {
    if (has_data && drop_data)
      drop_data--;
    else if (has_sack && drop_sack)
      drop_sack--;
    else
      drop_init--;
    dropped++;
    pthread_mutex_unlock(&wire_mutex);
    return (int)len;
  }
  assert(tail - head < QUEUE_CAPACITY - 1);
  const unsigned slot = tail++ % QUEUE_CAPACITY;
  packets[slot].destination = 1 - source;
  packets[slot].len = len;
  memcpy(packets[slot].data, data, len);
  if (has_data && duplicate_data) {
    duplicate_data--;
    packets[tail++ % QUEUE_CAPACITY] = packets[slot];
  }
  pthread_mutex_unlock(&wire_mutex);
  return (int)len;
}

static void assert_callback_owner(void* userdata) {
  Sctp* first = threaded ? threaded_endpoints[0] : &endpoints[0];
  Sctp* second = threaded ? threaded_endpoints[1] : &endpoints[1];
  assert(userdata == first || userdata == second);
  assert(pthread_equal(threaded ? owners[userdata == first ? 0 : 1] : owner, pthread_self()));
}

static void onmessage(char* data, size_t len, void* userdata, uint16_t sid) {
  assert_callback_owner(userdata);
  pthread_mutex_lock(&wire_mutex);
  assert(delivered < MAX_DELIVERIES);
  const uint8_t value = len ? (uint8_t)data[0] : 0;
  for (size_t i = 0; i < len; i++)
    assert((uint8_t)data[i] == value);
  deliveries[delivered++] = (Delivery){sid, len, value};
  pthread_mutex_unlock(&wire_mutex);
}

static void onopen(void* userdata) {
  assert_callback_owner(userdata);
  pthread_mutex_lock(&wire_mutex);
  opened++;
  pthread_mutex_unlock(&wire_mutex);
}

static void onclose(void* userdata) {
  assert_callback_owner(userdata);
  pthread_mutex_lock(&wire_mutex);
  closed++;
  pthread_mutex_unlock(&wire_mutex);
}

static void pump(void) {
  sctp_tick(&endpoints[0]);
  sctp_tick(&endpoints[1]);
  unsigned budget = QUEUE_CAPACITY;
  while (head != tail) {
    assert(budget-- > 0);
    const Packet packet = packets[head++ % QUEUE_CAPACITY];
    sctp_incoming_data(&endpoints[packet.destination], (char*)packet.data, packet.len);
  }
}

static void advance(unsigned milliseconds) {
  for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 10) {
    fake_ms += 10;
    sctp_tick(&endpoints[0]);
    sctp_tick(&endpoints[1]);
    pump();
  }
}

static void create_pair(void) {
  head = tail = delivered = 0;
  dropped = data_packets = dcep_acks = unordered_packets = opened = closed = forward_tsns = 0;
  drop_data = duplicate_data = drop_sack = 0;
  for (unsigned i = 0; i < 2; i++) {
    endpoints[i].userdata = &endpoints[i];
    sctp_onmessage(&endpoints[i], onmessage);
    sctp_onopen(&endpoints[i], onopen);
    sctp_onclose(&endpoints[i], onclose);
    assert(sctp_create_association(&endpoints[i], &transports[i]) == 0);
  }
  pump();
  for (unsigned elapsed = 0; opened < 2 && elapsed < 5000; elapsed += 10)
    advance(10);
  assert(opened == 2 && endpoints[0].connected && endpoints[1].connected);
}

static void destroy_pair(void) {
  sctp_destroy_association(&endpoints[0]);
  sctp_destroy_association(&endpoints[1]);
  head = tail;
  advance(2000);
  for (unsigned i = 0; i < 2; i++) {
    assert(!endpoints[i].connected && !endpoints[i].sock);
    assert(endpoints[i].stream_count == 0 && !endpoints[i].message_buf);
    assert(endpoints[i].dtls_srtp == NULL);
    sctp_destroy_association(&endpoints[i]);
  }
  assert(closed == 0);
}

static int send_message(unsigned source, uint16_t sid, uint8_t value, size_t len) {
  uint8_t data[SCTP_SEND_BUFFER_SIZE + 1];
  assert(len <= sizeof(data));
  memset(data, value, len);
  return sctp_outgoing_data(threaded ? threaded_endpoints[source] : &endpoints[source],
                            (char*)data, len, PPID_BINARY, sid);
}

static void test_loss_ordering_and_duplicates(void) {
  drop_init = 1;
  create_pair();
  assert(dropped == 1);
  drop_data = 1;
  assert(send_message(0, 0, 1, 32) == 32);
  for (uint8_t value = 2; value <= 3; value++)
    assert(send_message(0, 0, value, 32) == 32);
  pump();
  assert(delivered == 0);
  duplicate_data = 1;
  const uint64_t start = fake_ms;
  while (delivered < 3 && fake_ms - start < 1000)
    advance(10);
  assert(delivered == 3 && data_packets >= 4);
  for (unsigned i = 0; i < 3; i++) {
    assert(deliveries[i].sid == 0 && deliveries[i].value == i + 1);
    assert(deliveries[i].len == 32);
  }
  advance(2000);
  assert(delivered == 3);
  destroy_pair();
}

static void test_lost_sack_and_stream_independence(void) {
  create_pair();
  drop_data = 1;
  assert(send_message(0, 0, 1, 32) == 32);
  assert(send_message(0, 7, 2, 32) == 32);
  pump();
  assert(delivered == 1 && deliveries[0].sid == 7);
  drop_sack = 2;
  advance(2000);
  assert(delivered == 2 && deliveries[1].sid == 0);
  assert(dropped >= 2);
  destroy_pair();
}

static void test_gap_sack_fast_retransmit(void) {
  create_pair();
  drop_data = 1;
  for (uint8_t value = 1; value <= 8; value++)
    assert(send_message(0, 0, value, 32) == 32);
  const uint64_t start = fake_ms;
  pump();
  assert(fake_ms == start && delivered == 8 && data_packets >= 9);
  for (unsigned i = 0; i < 8; i++)
    assert(deliveries[i].value == i + 1);
  destroy_pair();
}

static void test_bounded_backpressure(void) {
  create_pair();
  int actual_buffer = 0;
  socklen_t size = sizeof(actual_buffer);
  assert(usrsctp_getsockopt(endpoints[0].sock, SOL_SOCKET, SO_SNDBUF, &actual_buffer, &size) == 0);
  assert(actual_buffer == SCTP_SEND_BUFFER_SIZE);
  unsigned accepted = 0;
  for (; accepted < 100; accepted++) {
    const int result = send_message(0, 0, 3, 1024);
    if (result < 0) {
      assert(errno == EWOULDBLOCK || errno == EAGAIN);
      break;
    }
    assert(result == 1024);
  }
  assert(accepted > 0 && accepted * 1024 <= SCTP_SEND_BUFFER_SIZE);
  assert(send_message(0, 0, 3, SCTP_SEND_BUFFER_SIZE + 1) == -1 && errno == EMSGSIZE);
  pump();
  advance(2000);
  assert(delivered == accepted);
  assert(send_message(0, 0, 4, 32) == 32);
  pump();
  advance(500);
  assert(delivered == accepted + 1 && deliveries[accepted].value == 4);
  destroy_pair();
}

static void send_open(uint16_t sid, const char* label, uint8_t type) {
  char data[64] = {DATA_CHANNEL_OPEN};
  const size_t label_len = strlen(label);
  assert(label_len + 12 <= sizeof(data));
  data[1] = (char)type;
  const uint16_t network_len = htons((uint16_t)label_len);
  memcpy(data + 8, &network_len, sizeof(network_len));
  memcpy(data + 12, label, label_len);
  assert(sctp_outgoing_data(&endpoints[0], data, 12 + label_len, PPID_CONTROL, sid) == (int)(12 + label_len));
  sctp_add_stream_mapping(&endpoints[0], label, sid);
  pump();
  advance(200);
}

static void test_channel_mappings(void) {
  create_pair();
  duplicate_data = 1;
  send_open(0, "input_channel_v1", 0);
  assert(dcep_acks == 1 && delivered == 0);
  send_open(0, "input_channel_v1", 0);
  assert(endpoints[0].stream_count == 1 && endpoints[1].stream_count == 1);
  assert(!strcmp(endpoints[1].stream_table[0].label, "input_channel_v1"));
  send_open(7, "unordered", 0x80);
  assert(endpoints[1].stream_table[1].sid == 7);
  assert(send_message(0, 7, 6, 32) == 32);
  pump();
  assert(delivered == 1 && deliveries[0].sid == 7 && unordered_packets == 1);
  assert(send_message(0, 0, 7, 32) == 32);
  pump();
  assert(delivered == 2 && deliveries[1].sid == 0 && unordered_packets == 1);
  send_open(9, "unreliable", 0x81);
  drop_data = 1;
  assert(send_message(0, 9, 11, 32) == 32);
  pump();
  advance(1000);
  assert(delivered == 2 && forward_tsns > 0);
  assert(send_message(0, 9, 12, 32) == 32);
  pump();
  assert(delivered == 3 && deliveries[2].value == 12);
  destroy_pair();
}

static void test_fragment_reassembly(void) {
  create_pair();
  drop_data = 1;
  duplicate_data = 1;
  assert(send_message(0, 0, 8, 12000) == 12000);
  pump();
  advance(3000);
  assert(delivered == 1 && deliveries[0].len == 12000 && deliveries[0].value == 8);
  const int large_buffer = 128 * 1024;
  assert(usrsctp_setsockopt(endpoints[0].sock, SOL_SOCKET, SO_SNDBUF, &large_buffer, sizeof(large_buffer)) == 0);
  uint8_t* large = malloc(SCTP_MAX_MESSAGE_SIZE + 1);
  assert(large);
  memset(large, 9, SCTP_MAX_MESSAGE_SIZE + 1);
  struct sctp_sndinfo info = {0};
  info.snd_ppid = htonl(PPID_BINARY);
  assert(usrsctp_sendv(endpoints[0].sock, large, SCTP_MAX_MESSAGE_SIZE, NULL, 0,
                       &info, sizeof(info), SCTP_SENDV_SNDINFO, 0) == SCTP_MAX_MESSAGE_SIZE);
  pump();
  advance(3000);
  assert(delivered == 2 && deliveries[1].len == SCTP_MAX_MESSAGE_SIZE && deliveries[1].value == 9);
  assert(usrsctp_sendv(endpoints[0].sock, large, SCTP_MAX_MESSAGE_SIZE + 1, NULL, 0,
                       &info, sizeof(info), SCTP_SENDV_SNDINFO, 0) == SCTP_MAX_MESSAGE_SIZE + 1);
  free(large);
  pump();
  advance(3000);
  assert(delivered == 2 && !endpoints[1].connected && !endpoints[1].sock);
  assert(closed >= 1);
  closed = 0;
  destroy_pair();
}

enum { WORK_CREATE = 1, WORK_TICK, WORK_INPUT, WORK_SEND, WORK_DESTROY, WORK_STOP };

typedef struct {
  Sctp sctp;
  DtlsSrtp dtls;
} PeerAllocation;

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  unsigned index;
  int command;
  int done;
  Packet packet;
  PeerAllocation* allocation;
} Worker;

static Worker workers[2];

static void* run_worker(void* userdata) {
  Worker* worker = userdata;
  const unsigned index = worker->index;
  owners[index] = pthread_self();
  for (;;) {
    pthread_mutex_lock(&worker->mutex);
    while (!worker->command)
      pthread_cond_wait(&worker->condition, &worker->mutex);
    const int command = worker->command;
    pthread_mutex_unlock(&worker->mutex);
    switch (command) {
      case WORK_CREATE:
        sctp_usrsctp_init();
        worker->allocation = calloc(1, sizeof(PeerAllocation));
        assert(worker->allocation);
        threaded_endpoints[index] = &worker->allocation->sctp;
        threaded_transports[index] = &worker->allocation->dtls;
        threaded_endpoints[index]->userdata = threaded_endpoints[index];
        sctp_onopen(threaded_endpoints[index], onopen);
        sctp_onclose(threaded_endpoints[index], onclose);
        sctp_onmessage(threaded_endpoints[index], onmessage);
        assert(sctp_create_association(threaded_endpoints[index], threaded_transports[index]) == 0);
        break;
      case WORK_TICK:
        sctp_tick(threaded_endpoints[index]);
        break;
      case WORK_INPUT:
        sctp_incoming_data(threaded_endpoints[index], (char*)worker->packet.data, worker->packet.len);
        break;
      case WORK_SEND:
        assert(send_message(index, (uint16_t)index, (uint8_t)(index + 20), 32) == 32);
        break;
      case WORK_DESTROY:
        sctp_destroy_association(threaded_endpoints[index]);
        free(worker->allocation);
        worker->allocation = NULL;
        sctp_usrsctp_deinit();
        break;
      default:
        assert(command == WORK_STOP);
        return NULL;
    }
    pthread_mutex_lock(&worker->mutex);
    worker->command = 0;
    worker->done = 1;
    pthread_cond_signal(&worker->condition);
    pthread_mutex_unlock(&worker->mutex);
  }
}

static void start_work(unsigned index, int command) {
  Worker* worker = &workers[index];
  pthread_mutex_lock(&worker->mutex);
  assert(!worker->command);
  worker->done = 0;
  worker->command = command;
  pthread_cond_signal(&worker->condition);
  pthread_mutex_unlock(&worker->mutex);
}

static void finish_work(unsigned index) {
  Worker* worker = &workers[index];
  pthread_mutex_lock(&worker->mutex);
  while (!worker->done)
    pthread_cond_wait(&worker->condition, &worker->mutex);
  pthread_mutex_unlock(&worker->mutex);
}

static void work_both(int command) {
  start_work(0, command);
  start_work(1, command);
  finish_work(0);
  finish_work(1);
}

static void pump_workers(void) {
  work_both(WORK_TICK);
  unsigned budget = QUEUE_CAPACITY;
  for (;;) {
    pthread_mutex_lock(&wire_mutex);
    if (head == tail) {
      pthread_mutex_unlock(&wire_mutex);
      break;
    }
    const Packet packet = packets[head++ % QUEUE_CAPACITY];
    pthread_mutex_unlock(&wire_mutex);
    assert(budget-- > 0);
    workers[packet.destination].packet = packet;
    start_work(packet.destination, WORK_INPUT);
    finish_work(packet.destination);
  }
}

static void test_overlapping_owner_threads(void) {
  head = tail = delivered = opened = closed = 0;
  drop_data = duplicate_data = drop_init = drop_sack = 0;
  threaded = 1;
  pthread_t threads[2];
  for (unsigned i = 0; i < 2; i++) {
    workers[i].index = i;
    assert(pthread_mutex_init(&workers[i].mutex, NULL) == 0);
    assert(pthread_cond_init(&workers[i].condition, NULL) == 0);
    assert(pthread_create(&threads[i], NULL, run_worker, &workers[i]) == 0);
  }
  work_both(WORK_CREATE);
  pump_workers();
  assert(opened == 2);
  drop_data = 1;
  start_work(1, WORK_SEND);
  finish_work(1);
  start_work(1, WORK_TICK);
  finish_work(1);
  const unsigned sent_before_timer = data_packets;
  fake_ms += 500;
  start_work(0, WORK_TICK);
  finish_work(0);
  assert(data_packets == sent_before_timer && delivered == 0);
  start_work(1, WORK_TICK);
  finish_work(1);
  assert(data_packets > sent_before_timer);
  pump_workers();
  assert(delivered == 1 && deliveries[0].value == 21);
  for (unsigned i = 0; i < 30; i++) {
    drop_data = i % 7 == 0;
    work_both(WORK_SEND);
    pump_workers();
    fake_ms += 500;
    pump_workers();
  }
  for (unsigned i = 0; delivered < 61 && i < 500; i++) {
    fake_ms += 10;
    pump_workers();
  }
  assert(delivered == 61);
  drop_data = 1;
  start_work(1, WORK_SEND);
  finish_work(1);
  start_work(1, WORK_TICK);
  finish_work(1);
  fake_ms += 500;
  start_work(0, WORK_TICK);
  start_work(1, WORK_DESTROY);
  finish_work(0);
  finish_work(1);
  assert(!workers[1].allocation);
  head = tail;
  for (unsigned i = 0; i < 10; i++) {
    fake_ms += 1000;
    start_work(0, WORK_TICK);
    finish_work(0);
  }
  start_work(0, WORK_DESTROY);
  finish_work(0);
  for (unsigned i = 0; i < 2; i++) {
    start_work(i, WORK_STOP);
    assert(pthread_join(threads[i], NULL) == 0);
    pthread_cond_destroy(&workers[i].condition);
    pthread_mutex_destroy(&workers[i].mutex);
  }
  threaded = 0;
}

int main(void) {
  owner = pthread_self();
  sctp_usrsctp_init();
  test_loss_ordering_and_duplicates();
  test_lost_sack_and_stream_independence();
  test_gap_sack_fast_retransmit();
  test_bounded_backpressure();
  test_channel_mappings();
  test_fragment_reassembly();
  fake_ms = UINT32_MAX - 30u;
  test_loss_ordering_and_duplicates();
  sctp_usrsctp_deinit();
  sctp_usrsctp_init();
  create_pair();
  assert(send_message(0, 0, 10, 32) == 32);
  pump();
  assert(delivered == 1);
  destroy_pair();
  sctp_usrsctp_deinit();
  sctp_usrsctp_deinit();
  test_overlapping_owner_threads();
  puts("SCTP packet tests passed: loss, duplication, ordering, streams, bounds, reassembly, reconnect and teardown");
  return 0;
}
