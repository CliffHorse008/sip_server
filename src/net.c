#include "net.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int sockaddr_from_ip_port(const char *ip, uint16_t port, struct sockaddr_in *addr)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr->sin_addr) != 1) {
        return -1;
    }

    return 0;
}

int udp_socket_bind(const char *ip, uint16_t port)
{
    int fd;
    int reuse = 1;
    struct sockaddr_in addr;

    if (sockaddr_from_ip_port(ip, port, &addr) != 0) {
        fprintf(stderr, "invalid bind address: %s:%u\n", ip, port);
        return -1;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    return fd;
}

void sockaddr_to_ip_string(const struct sockaddr_in *addr, char *buffer, size_t buffer_size)
{
    if (inet_ntop(AF_INET, &addr->sin_addr, buffer, (socklen_t) buffer_size) == NULL) {
        snprintf(buffer, buffer_size, "0.0.0.0");
    }
}
