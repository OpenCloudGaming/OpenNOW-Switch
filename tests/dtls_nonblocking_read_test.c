#include <assert.h>
#include <string.h>

#include "dtls_srtp.h"

static int read_calls;
static int read_result;

int mbedtls_ssl_read(mbedtls_ssl_context* ssl, unsigned char* buffer, size_t length) {
  (void)ssl;
  assert(++read_calls == 1);
  if (read_result > 0) {
    assert((size_t)read_result <= length);
    memset(buffer, 0x42, read_result);
  }
  return read_result;
}

int main(void) {
  DtlsSrtp dtls = {0};
  const int results[] = {
    MBEDTLS_ERR_SSL_WANT_READ,
    MBEDTLS_ERR_SSL_WANT_WRITE,
    MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY,
    MBEDTLS_ERR_SSL_INVALID_RECORD,
    0,
    7,
  };
  for (size_t index = 0; index < sizeof(results) / sizeof(results[0]); ++index) {
    unsigned char buffer[32];
    memset(buffer, 0xff, sizeof(buffer));
    read_calls = 0;
    read_result = results[index];
    assert(dtls_srtp_read(&dtls, buffer, sizeof(buffer)) == read_result);
    assert(read_calls == 1);
    if (read_result > 0)
      assert(buffer[0] == 0x42 && buffer[read_result - 1] == 0x42);
  }
}
