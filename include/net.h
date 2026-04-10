#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <netinet/in.h>

/* 创建并绑定一个 UDP socket。 */
int udp_socket_bind(const char *ip, uint16_t port);
/* 将 IP 和端口转换为 sockaddr_in。 */
int sockaddr_from_ip_port(const char *ip, uint16_t port, struct sockaddr_in *addr);
/* 将 sockaddr_in 中的地址格式化为字符串。 */
void sockaddr_to_ip_string(const struct sockaddr_in *addr, char *buffer, size_t buffer_size);

#endif
