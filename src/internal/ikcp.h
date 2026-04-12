#ifndef SIPSERVER_IKCP_H
#define SIPSERVER_IKCP_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t IUINT8;
typedef uint16_t IUINT16;
typedef uint32_t IUINT32;
typedef int32_t IINT32;

typedef struct IKCPCB ikcpcb;

#define IKCP_OVERHEAD 24U

ikcpcb *ikcp_create(IUINT32 conv, void *user);
void ikcp_release(ikcpcb *kcp);
void ikcp_setoutput(ikcpcb *kcp, int (*output)(const char *buf, int len, ikcpcb *kcp, void *user));

int ikcp_recv(ikcpcb *kcp, char *buffer, int len);
int ikcp_send(ikcpcb *kcp, const char *buffer, int len);
void ikcp_update(ikcpcb *kcp, IUINT32 current);
IUINT32 ikcp_check(const ikcpcb *kcp, IUINT32 current);
int ikcp_input(ikcpcb *kcp, const char *data, long size);
void ikcp_flush(ikcpcb *kcp);
int ikcp_peeksize(const ikcpcb *kcp);
int ikcp_setmtu(ikcpcb *kcp, int mtu);
int ikcp_wndsize(ikcpcb *kcp, int sndwnd, int rcvwnd);
int ikcp_waitsnd(const ikcpcb *kcp);
int ikcp_nodelay(ikcpcb *kcp, int nodelay, int interval, int resend, int nc);
void ikcp_log(ikcpcb *kcp, int mask, const char *fmt, ...);
void ikcp_allocator(void *(*new_malloc)(size_t), void (*new_free)(void *));
IUINT32 ikcp_getconv(const void *ptr);

#endif
