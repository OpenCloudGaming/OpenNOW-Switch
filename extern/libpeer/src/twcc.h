#ifndef TWCC_H_
#define TWCC_H_

#include <stddef.h>
#include <stdint.h>

#define TWCC_EXTENSION_URI "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"
#define TWCC_HISTORY_SIZE 2048
#define TWCC_FEEDBACK_INTERVAL_US 50000
#define TWCC_MAX_FEEDBACK_SIZE 1200

typedef struct TwccReceiver {
  uint8_t extension_id;
  uint8_t feedback_count;
  uint8_t has_packets;
  uint32_t media_ssrc;
  int64_t next_sequence;
  int64_t max_sequence;
  uint64_t last_feedback_us;
  uint32_t packets_recorded;
  uint32_t late_packets;
  uint32_t feedback_sent;
  uint64_t arrival_us[TWCC_HISTORY_SIZE];
  uint8_t received[TWCC_HISTORY_SIZE];
} TwccReceiver;

int twcc_parse_extension_id(const char* sdp_line);
int twcc_read_sequence(const uint8_t* packet, size_t size, uint8_t extension_id, uint16_t* sequence);
void twcc_receiver_init(TwccReceiver* receiver, uint8_t extension_id);
int twcc_receiver_record(TwccReceiver* receiver, const uint8_t* packet, size_t size, uint64_t arrival_us);
int twcc_receiver_feedback_due(const TwccReceiver* receiver, uint64_t now_us);
int twcc_receiver_build_feedback(TwccReceiver* receiver, uint32_t sender_ssrc,
                                 uint8_t* packet, size_t capacity);
void twcc_receiver_feedback_sent(TwccReceiver* receiver, uint64_t now_us);

#endif  // TWCC_H_
