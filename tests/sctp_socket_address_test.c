#include <assert.h>
#include <arpa/inet.h>
#include <stddef.h>
#include <string.h>
#include <usrsctp.h>

_Static_assert(offsetof(struct sockaddr_conn, sconn_family) ==
               offsetof(struct sockaddr, sa_family),
               "SCTP address family must match the platform socket ABI");
_Static_assert(sizeof(((struct sockaddr_conn*)0)->sconn_family) ==
               sizeof(((struct sockaddr*)0)->sa_family),
               "SCTP address family width must match the platform socket ABI");

int main(void) {
  struct sockaddr_conn address = {0};
  address.sconn_family = AF_CONN;
  address.sconn_port = htons(5000);
  address.sconn_addr = &address;
  struct sockaddr generic = {0};
  memcpy(&generic, &address, offsetof(struct sockaddr, sa_data));
  assert(generic.sa_family == AF_CONN);
  return 0;
}
