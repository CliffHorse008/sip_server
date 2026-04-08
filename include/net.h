#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <netinet/in.h>

int udp_socket_bind(const char *ip, uint16_t port);
int sockaddr_from_ip_port(const char *ip, uint16_t port, struct sockaddr_in *addr);
void sockaddr_to_ip_string(const struct sockaddr_in *addr, char *buffer, size_t buffer_size);

#endif
