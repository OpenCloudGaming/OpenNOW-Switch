#include <assert.h>
#include <stdint.h>
#include <string.h>

uint32_t calculate_crc32c(uint32_t crc32c, const unsigned char* buffer, unsigned int length);
uint32_t sctp_finalize_crc32c(uint32_t crc32c);

static void assert_wire_checksum(const unsigned char* data, unsigned int length, uint32_t expected) {
  const uint32_t stored = sctp_finalize_crc32c(calculate_crc32c(0xffffffff, data, length));
  unsigned char wire[4];
  memcpy(wire, &stored, sizeof(wire));
  assert(wire[0] == (expected & 0xff));
  assert(wire[1] == ((expected >> 8) & 0xff));
  assert(wire[2] == ((expected >> 16) & 0xff));
  assert(wire[3] == (expected >> 24));
}

int main(void) {
  const unsigned char check[] = "123456789";
  unsigned char init[44] = {0x13, 0x88, 0x13, 0x88, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0x20};
  for (unsigned i = 16; i < sizeof(init); ++i)
    init[i] = (unsigned char)(i - 16);
  assert_wire_checksum(check, 9, 0xe3069283u);
  assert_wire_checksum(init, sizeof(init), 0x3e2be4f4u);
  return 0;
}
