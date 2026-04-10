#ifndef SIPSERVER_INTERNAL_NET_H
#define SIPSERVER_INTERNAL_NET_H

#include <netinet/in.h>
#include <stdint.h>

int udp_socket_bind(const char *ip, uint16_t port);
int sockaddr_from_ip_port(const char *ip, uint16_t port, struct sockaddr_in *addr);
void sockaddr_to_ip_string(const struct sockaddr_in *addr, char *buffer, size_t buffer_size);

#endif
