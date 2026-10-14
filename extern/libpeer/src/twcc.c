#include "twcc.h"

#include <stdlib.h>
#include <string.h>

#define TWCC_HISTORY_MASK (TWCC_HISTORY_SIZE - 1)
#define TWCC_HEADER_SIZE 20
#define TWCC_DELTA_TICK_US 250
#define TWCC_REFERENCE_TICK_US 64000

_Static_assert((TWCC_HISTORY_SIZE & TWCC_HISTORY_MASK) == 0, "TWCC history must be a power of two");

static uint16_t twcc_read_u16(const uint8_t* data) {
  return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t twcc_read_u32(const uint8_t* data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | data[3];
}

static void twcc_write_u16(uint8_t* data, uint16_t value) {
  data[0] = (uint8_t)(value >> 8);
  data[1] = (uint8_t)value;
}

static void twcc_write_u32(uint8_t* data, uint32_t value) {
  data[0] = (uint8_t)(value >> 24);
  data[1] = (uint8_t)(value >> 16);
  data[2] = (uint8_t)(value >> 8);
  data[3] = (uint8_t)value;
}

int twcc_parse_extension_id(const char* sdp_line) {
  static const char prefix[] = "a=extmap:";
  if (!sdp_line || strncmp(sdp_line, prefix, sizeof(prefix) - 1) != 0 ||
      !strstr(sdp_line, TWCC_EXTENSION_URI))
    return 0;
  char* end = NULL;
  const long id = strtol(sdp_line + sizeof(prefix) - 1, &end, 10);
  if (end == sdp_line + sizeof(prefix) - 1 || (*end != ' ' && *end != '/'))
    return 0;
  return id >= 1 && id <= 255 && id != 15 ? (int)id : 0;
}

int twcc_read_sequence(const uint8_t* packet, size_t size, uint8_t extension_id, uint16_t* sequence) {
  if (!packet || extension_id == 0 || size < 12 || (packet[0] & 0xC0) != 0x80 || !(packet[0] & 0x10))
    return 0;
  size_t pos = 12 + 4 * (size_t)(packet[0] & 0x0F);
  if (pos + 4 > size)
    return 0;
  const uint16_t profile = twcc_read_u16(packet + pos);
  const size_t end = pos + 4 + 4 * (size_t)twcc_read_u16(packet + pos + 2);
  pos += 4;
  if (end > size)
    return 0;

  const int one_byte = profile == 0xBEDE;
  if (!one_byte && (profile & 0xFFF0) != 0x1000)
    return 0;
  while (pos < end) {
    if (packet[pos] == 0) {
      pos++;
      continue;
    }
    uint8_t id;
    size_t length;
    if (one_byte) {
      id = packet[pos] >> 4;
      length = (size_t)(packet[pos] & 0x0F) + 1;
      if (id == 15)
        return 0;
      pos++;
    } else {
      if (pos + 2 > end)
        return 0;
      id = packet[pos];
      length = packet[pos + 1];
      pos += 2;
    }
    if (pos + length > end)
      return 0;
    if (id == extension_id) {
      if (length != 2)
        return 0;
      *sequence = twcc_read_u16(packet + pos);
      return 1;
    }
    pos += length;
  }
  return 0;
}

void twcc_receiver_init(TwccReceiver* receiver, uint8_t extension_id) {
  memset(receiver, 0, sizeof(*receiver));
  receiver->extension_id = extension_id;
}

int twcc_receiver_record(TwccReceiver* receiver, const uint8_t* packet, size_t size, uint64_t arrival_us) {
  uint16_t sequence;
  if (!twcc_read_sequence(packet, size, receiver->extension_id, &sequence))
    return 0;

  if (!receiver->has_packets) {
    receiver->next_sequence = (int64_t)sequence + 0x10000;
    receiver->max_sequence = receiver->next_sequence - 1;
    receiver->has_packets = 1;
  }
  const int64_t unwrapped =
      receiver->max_sequence + (int16_t)(uint16_t)(sequence - (uint16_t)receiver->max_sequence);
  if (unwrapped < receiver->next_sequence) {
    receiver->late_packets++;
    return 0;
  }
  if (unwrapped > receiver->max_sequence) {
    int64_t clear_from = receiver->max_sequence + 1;
    if (unwrapped - clear_from >= TWCC_HISTORY_SIZE)
      clear_from = unwrapped - TWCC_HISTORY_SIZE + 1;
    for (int64_t s = clear_from; s <= unwrapped; s++)
      receiver->received[s & TWCC_HISTORY_MASK] = 0;
    receiver->max_sequence = unwrapped;
    if (receiver->max_sequence - receiver->next_sequence >= TWCC_HISTORY_SIZE)
      receiver->next_sequence = receiver->max_sequence - TWCC_HISTORY_SIZE + 1;
  }

  const size_t slot = (size_t)(unwrapped & TWCC_HISTORY_MASK);
  if (receiver->received[slot])
    return 0;
  receiver->received[slot] = 1;
  receiver->arrival_us[slot] = arrival_us;
  receiver->media_ssrc = twcc_read_u32(packet + 8);
  receiver->packets_recorded++;
  return 1;
}

int twcc_receiver_feedback_due(const TwccReceiver* receiver, uint64_t now_us) {
  return receiver->has_packets && receiver->next_sequence <= receiver->max_sequence &&
         now_us - receiver->last_feedback_us >= TWCC_FEEDBACK_INTERVAL_US;
}

static size_t twcc_padded_size(size_t status_count, size_t delta_size) {
  const size_t size = TWCC_HEADER_SIZE + ((status_count + 6) / 7) * 2 + delta_size;
  return (size + 3) & ~(size_t)3;
}

int twcc_receiver_build_feedback(TwccReceiver* receiver, uint32_t sender_ssrc,
                                 uint8_t* packet, size_t capacity) {
  if (!receiver->has_packets || receiver->next_sequence > receiver->max_sequence)
    return 0;
  if (capacity > TWCC_MAX_FEEDBACK_SIZE)
    capacity = TWCC_MAX_FEEDBACK_SIZE;
  if (capacity < twcc_padded_size(1, 2))
    return -1;

  const int64_t base = receiver->next_sequence;
  int64_t first_received = base;
  while (!receiver->received[first_received & TWCC_HISTORY_MASK])
    first_received++;
  const uint64_t reference = receiver->arrival_us[first_received & TWCC_HISTORY_MASK] /
                             TWCC_REFERENCE_TICK_US;
  int64_t last_us = (int64_t)(reference * TWCC_REFERENCE_TICK_US);

  uint8_t symbols[TWCC_HISTORY_SIZE];
  uint8_t deltas[TWCC_MAX_FEEDBACK_SIZE];
  size_t count = 0;
  size_t delta_size = 0;
  for (int64_t s = base; s <= receiver->max_sequence; s++) {
    const size_t slot = (size_t)(s & TWCC_HISTORY_MASK);
    uint8_t symbol = 0;
    int64_t ticks = 0;
    if (receiver->received[slot]) {
      const int64_t elapsed = (int64_t)receiver->arrival_us[slot] - last_us;
      ticks = elapsed >= 0 ? (elapsed + TWCC_DELTA_TICK_US / 2) / TWCC_DELTA_TICK_US
                           : -((-elapsed + TWCC_DELTA_TICK_US / 2) / TWCC_DELTA_TICK_US);
      if (ticks < INT16_MIN || ticks > INT16_MAX)
        break;
      symbol = ticks >= 0 && ticks <= 255 ? 1 : 2;
    }
    if (twcc_padded_size(count + 1, delta_size + symbol) > capacity)
      break;
    symbols[count++] = symbol;
    if (symbol == 1) {
      deltas[delta_size++] = (uint8_t)ticks;
    } else if (symbol == 2) {
      twcc_write_u16(deltas + delta_size, (uint16_t)(int16_t)ticks);
      delta_size += 2;
    }
    if (symbol)
      last_us += ticks * TWCC_DELTA_TICK_US;
  }

  const size_t unpadded = TWCC_HEADER_SIZE + ((count + 6) / 7) * 2 + delta_size;
  const size_t total = twcc_padded_size(count, delta_size);
  const size_t padding = total - unpadded;
  packet[0] = (uint8_t)(0x80 | (padding ? 0x20 : 0) | 15);
  packet[1] = 205;
  twcc_write_u16(packet + 2, (uint16_t)(total / 4 - 1));
  twcc_write_u32(packet + 4, sender_ssrc);
  twcc_write_u32(packet + 8, receiver->media_ssrc);
  twcc_write_u16(packet + 12, (uint16_t)base);
  twcc_write_u16(packet + 14, (uint16_t)count);
  packet[16] = (uint8_t)(reference >> 16);
  packet[17] = (uint8_t)(reference >> 8);
  packet[18] = (uint8_t)reference;
  packet[19] = receiver->feedback_count++;

  size_t pos = TWCC_HEADER_SIZE;
  for (size_t first = 0; first < count; first += 7) {
    uint16_t chunk = 0xC000;
    for (size_t i = 0; i < 7 && first + i < count; i++)
      chunk |= (uint16_t)(symbols[first + i] << (12 - 2 * i));
    twcc_write_u16(packet + pos, chunk);
    pos += 2;
  }
  memcpy(packet + pos, deltas, delta_size);
  pos += delta_size;
  if (padding) {
    memset(packet + pos, 0, padding);
    packet[total - 1] = (uint8_t)padding;
  }

  receiver->next_sequence = base + (int64_t)count;
  return (int)total;
}

void twcc_receiver_feedback_sent(TwccReceiver* receiver, uint64_t now_us) {
  receiver->last_feedback_us = now_us;
  receiver->feedback_sent++;
}
