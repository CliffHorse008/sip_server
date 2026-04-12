#include "internal/ikcp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IKCP_CMD_PUSH 81U
#define IKCP_CMD_ACK 82U
#define IKCP_MTU_DEF 1400U
#define IKCP_INTERVAL_DEF 20U
#define IKCP_RTO_DEF 200U

typedef struct kcp_segment {
    struct kcp_segment *next;
    IUINT32 sn;
    IUINT32 ts;
    IUINT32 resendts;
    IUINT32 len;
    unsigned int xmit;
    char *data;
} kcp_segment_t;

typedef struct kcp_ack {
    struct kcp_ack *next;
    IUINT32 sn;
    IUINT32 ts;
} kcp_ack_t;

struct IKCPCB {
    IUINT32 conv;
    IUINT32 mtu;
    IUINT32 mss;
    IUINT32 snd_nxt;
    IUINT32 snd_una;
    IUINT32 rcv_nxt;
    IUINT32 current;
    IUINT32 interval;
    IUINT32 ts_flush;
    IUINT32 rx_rto;
    int updated;
    void *user;
    int (*output)(const char *buf, int len, ikcpcb *kcp, void *user);
    kcp_segment_t *snd_queue_head;
    kcp_segment_t *snd_queue_tail;
    kcp_segment_t *snd_buf_head;
    kcp_segment_t *snd_buf_tail;
    kcp_segment_t *rcv_queue_head;
    kcp_segment_t *rcv_queue_tail;
    kcp_segment_t *rcv_buf_head;
    kcp_ack_t *ack_head;
    kcp_ack_t *ack_tail;
};

static void *(*g_malloc_fn)(size_t) = malloc;
static void (*g_free_fn)(void *) = free;

static char *encode8u(char *p, IUINT8 value)
{
    *p++ = (char) value;
    return p;
}

static char *encode16u(char *p, IUINT16 value)
{
    *p++ = (char) (value & 0xFFU);
    *p++ = (char) ((value >> 8) & 0xFFU);
    return p;
}

static char *encode32u(char *p, IUINT32 value)
{
    *p++ = (char) (value & 0xFFU);
    *p++ = (char) ((value >> 8) & 0xFFU);
    *p++ = (char) ((value >> 16) & 0xFFU);
    *p++ = (char) ((value >> 24) & 0xFFU);
    return p;
}

static const char *decode8u(const char *p, IUINT8 *value)
{
    *value = (IUINT8) (unsigned char) *p++;
    return p;
}

static const char *decode16u(const char *p, IUINT16 *value)
{
    *value = (IUINT16) (unsigned char) p[0] |
             (IUINT16) ((IUINT16) (unsigned char) p[1] << 8);
    return p + 2;
}

static const char *decode32u(const char *p, IUINT32 *value)
{
    *value = (IUINT32) (unsigned char) p[0] |
             ((IUINT32) (unsigned char) p[1] << 8) |
             ((IUINT32) (unsigned char) p[2] << 16) |
             ((IUINT32) (unsigned char) p[3] << 24);
    return p + 4;
}

static long timediff(IUINT32 later, IUINT32 earlier)
{
    return (long) ((IINT32) (later - earlier));
}

static void free_segment(kcp_segment_t *segment)
{
    if (segment == NULL) {
        return;
    }

    g_free_fn(segment->data);
    g_free_fn(segment);
}

static void append_segment(kcp_segment_t **head, kcp_segment_t **tail, kcp_segment_t *segment)
{
    segment->next = NULL;
    if (*tail == NULL) {
        *head = segment;
    } else {
        (*tail)->next = segment;
    }
    *tail = segment;
}

static kcp_segment_t *pop_segment(kcp_segment_t **head, kcp_segment_t **tail)
{
    kcp_segment_t *segment = *head;

    if (segment == NULL) {
        return NULL;
    }

    *head = segment->next;
    if (*head == NULL) {
        *tail = NULL;
    }
    segment->next = NULL;
    return segment;
}

static void append_ack(ikcpcb *kcp, IUINT32 sn, IUINT32 ts)
{
    kcp_ack_t *ack = (kcp_ack_t *) g_malloc_fn(sizeof(*ack));

    if (ack == NULL) {
        return;
    }

    ack->next = NULL;
    ack->sn = sn;
    ack->ts = ts;
    if (kcp->ack_tail == NULL) {
        kcp->ack_head = ack;
    } else {
        kcp->ack_tail->next = ack;
    }
    kcp->ack_tail = ack;
}

static void refresh_snd_una(ikcpcb *kcp)
{
    kcp->snd_una = kcp->snd_buf_head != NULL ? kcp->snd_buf_head->sn : kcp->snd_nxt;
}

static void flush_one_segment(ikcpcb *kcp, IUINT8 cmd, IUINT32 sn, IUINT32 ts, const char *data, IUINT32 len)
{
    char *packet;
    char *cursor;

    if (kcp->output == NULL) {
        return;
    }

    packet = (char *) g_malloc_fn(IKCP_OVERHEAD + len);
    if (packet == NULL) {
        return;
    }

    cursor = packet;
    cursor = encode32u(cursor, kcp->conv);
    cursor = encode8u(cursor, cmd);
    cursor = encode8u(cursor, 0);
    cursor = encode16u(cursor, 0);
    cursor = encode32u(cursor, ts);
    cursor = encode32u(cursor, sn);
    cursor = encode32u(cursor, kcp->snd_una);
    cursor = encode32u(cursor, len);
    if (len != 0 && data != NULL) {
        memcpy(cursor, data, len);
    }

    kcp->output(packet, (int) (IKCP_OVERHEAD + len), kcp, kcp->user);
    g_free_fn(packet);
}

static void move_rcv_buf_to_queue(ikcpcb *kcp)
{
    while (kcp->rcv_buf_head != NULL && kcp->rcv_buf_head->sn == kcp->rcv_nxt) {
        kcp_segment_t *segment = kcp->rcv_buf_head;

        kcp->rcv_buf_head = segment->next;
        segment->next = NULL;
        append_segment(&kcp->rcv_queue_head, &kcp->rcv_queue_tail, segment);
        ++kcp->rcv_nxt;
    }
}

static void insert_rcv_buf(ikcpcb *kcp, kcp_segment_t *segment)
{
    kcp_segment_t *prev = NULL;
    kcp_segment_t *cursor = kcp->rcv_buf_head;

    while (cursor != NULL && timediff(cursor->sn, segment->sn) < 0) {
        prev = cursor;
        cursor = cursor->next;
    }

    if (cursor != NULL && cursor->sn == segment->sn) {
        free_segment(segment);
        return;
    }

    if (prev == NULL) {
        segment->next = kcp->rcv_buf_head;
        kcp->rcv_buf_head = segment;
    } else {
        segment->next = cursor;
        prev->next = segment;
    }

    move_rcv_buf_to_queue(kcp);
}

static void remove_acked_segments(ikcpcb *kcp, IUINT32 ack_sn, IUINT32 una)
{
    kcp_segment_t *prev = NULL;
    kcp_segment_t *cursor = kcp->snd_buf_head;

    while (cursor != NULL) {
        int acked = cursor->sn == ack_sn || timediff(una, cursor->sn + 1U) >= 0;
        kcp_segment_t *next = cursor->next;

        if (acked) {
            if (prev == NULL) {
                kcp->snd_buf_head = next;
            } else {
                prev->next = next;
            }
            if (kcp->snd_buf_tail == cursor) {
                kcp->snd_buf_tail = prev;
            }
            free_segment(cursor);
        } else {
            prev = cursor;
        }

        cursor = next;
    }

    refresh_snd_una(kcp);
}

ikcpcb *ikcp_create(IUINT32 conv, void *user)
{
    ikcpcb *kcp = (ikcpcb *) g_malloc_fn(sizeof(*kcp));

    if (kcp == NULL) {
        return NULL;
    }

    memset(kcp, 0, sizeof(*kcp));
    kcp->conv = conv;
    kcp->user = user;
    kcp->mtu = IKCP_MTU_DEF;
    kcp->mss = IKCP_MTU_DEF - IKCP_OVERHEAD;
    kcp->interval = IKCP_INTERVAL_DEF;
    kcp->ts_flush = IKCP_INTERVAL_DEF;
    kcp->rx_rto = IKCP_RTO_DEF;
    return kcp;
}

void ikcp_release(ikcpcb *kcp)
{
    kcp_segment_t *segment;
    kcp_ack_t *ack;

    if (kcp == NULL) {
        return;
    }

    while ((segment = pop_segment(&kcp->snd_queue_head, &kcp->snd_queue_tail)) != NULL) {
        free_segment(segment);
    }
    while ((segment = pop_segment(&kcp->snd_buf_head, &kcp->snd_buf_tail)) != NULL) {
        free_segment(segment);
    }
    while ((segment = pop_segment(&kcp->rcv_queue_head, &kcp->rcv_queue_tail)) != NULL) {
        free_segment(segment);
    }
    while ((segment = kcp->rcv_buf_head) != NULL) {
        kcp->rcv_buf_head = segment->next;
        free_segment(segment);
    }
    while ((ack = kcp->ack_head) != NULL) {
        kcp->ack_head = ack->next;
        g_free_fn(ack);
    }

    g_free_fn(kcp);
}

void ikcp_setoutput(ikcpcb *kcp, int (*output)(const char *buf, int len, ikcpcb *kcp, void *user))
{
    if (kcp != NULL) {
        kcp->output = output;
    }
}

int ikcp_recv(ikcpcb *kcp, char *buffer, int len)
{
    kcp_segment_t *segment;

    if (kcp == NULL || len < 0) {
        return -1;
    }

    segment = pop_segment(&kcp->rcv_queue_head, &kcp->rcv_queue_tail);
    if (segment == NULL) {
        return -1;
    }

    if ((IUINT32) len < segment->len) {
        append_segment(&kcp->rcv_queue_head, &kcp->rcv_queue_tail, segment);
        return -3;
    }

    memcpy(buffer, segment->data, segment->len);
    len = (int) segment->len;
    free_segment(segment);
    return len;
}

int ikcp_send(ikcpcb *kcp, const char *buffer, int len)
{
    kcp_segment_t *segment;

    if (kcp == NULL || buffer == NULL || len < 0 || (IUINT32) len > kcp->mss) {
        return -1;
    }

    segment = (kcp_segment_t *) g_malloc_fn(sizeof(*segment));
    if (segment == NULL) {
        return -2;
    }

    memset(segment, 0, sizeof(*segment));
    segment->data = (char *) g_malloc_fn((size_t) len);
    if (segment->data == NULL) {
        g_free_fn(segment);
        return -2;
    }

    memcpy(segment->data, buffer, (size_t) len);
    segment->len = (IUINT32) len;
    append_segment(&kcp->snd_queue_head, &kcp->snd_queue_tail, segment);
    return len;
}

void ikcp_flush(ikcpcb *kcp)
{
    kcp_ack_t *ack;
    kcp_segment_t *segment;

    if (kcp == NULL || !kcp->updated) {
        return;
    }

    while ((ack = kcp->ack_head) != NULL) {
        kcp->ack_head = ack->next;
        if (kcp->ack_head == NULL) {
            kcp->ack_tail = NULL;
        }
        flush_one_segment(kcp, IKCP_CMD_ACK, ack->sn, ack->ts, NULL, 0);
        g_free_fn(ack);
    }

    while ((segment = pop_segment(&kcp->snd_queue_head, &kcp->snd_queue_tail)) != NULL) {
        segment->sn = kcp->snd_nxt++;
        segment->ts = kcp->current;
        segment->resendts = kcp->current + kcp->rx_rto;
        segment->xmit = 1;
        flush_one_segment(kcp, IKCP_CMD_PUSH, segment->sn, segment->ts, segment->data, segment->len);
        append_segment(&kcp->snd_buf_head, &kcp->snd_buf_tail, segment);
        refresh_snd_una(kcp);
    }

    for (segment = kcp->snd_buf_head; segment != NULL; segment = segment->next) {
        if (timediff(kcp->current, segment->resendts) >= 0) {
            segment->ts = kcp->current;
            segment->resendts = kcp->current + kcp->rx_rto;
            ++segment->xmit;
            flush_one_segment(kcp, IKCP_CMD_PUSH, segment->sn, segment->ts, segment->data, segment->len);
        }
    }
}

void ikcp_update(ikcpcb *kcp, IUINT32 current)
{
    if (kcp == NULL) {
        return;
    }

    kcp->current = current;
    if (!kcp->updated) {
        kcp->updated = 1;
        kcp->ts_flush = current;
    }

    if (timediff(current, kcp->ts_flush) >= 0) {
        kcp->ts_flush = current + kcp->interval;
        ikcp_flush(kcp);
    }
}

IUINT32 ikcp_check(const ikcpcb *kcp, IUINT32 current)
{
    IUINT32 next = current + (kcp != NULL ? kcp->interval : IKCP_INTERVAL_DEF);
    const kcp_segment_t *segment;

    if (kcp == NULL) {
        return next;
    }

    for (segment = kcp->snd_buf_head; segment != NULL; segment = segment->next) {
        if (timediff(segment->resendts, next) < 0) {
            next = segment->resendts;
        }
    }

    return next;
}

int ikcp_input(ikcpcb *kcp, const char *data, long size)
{
    while (kcp != NULL && data != NULL && size >= (long) IKCP_OVERHEAD) {
        IUINT32 conv;
        IUINT32 ts;
        IUINT32 sn;
        IUINT32 una;
        IUINT32 len;
        IUINT16 wnd;
        IUINT8 cmd;
        IUINT8 frg;
        kcp_segment_t *segment;

        data = decode32u(data, &conv);
        data = decode8u(data, &cmd);
        data = decode8u(data, &frg);
        data = decode16u(data, &wnd);
        data = decode32u(data, &ts);
        data = decode32u(data, &sn);
        data = decode32u(data, &una);
        data = decode32u(data, &len);
        size -= (long) IKCP_OVERHEAD;
        (void) wnd;
        (void) frg;

        if (conv != kcp->conv || size < (long) len) {
            return -1;
        }

        if (cmd == IKCP_CMD_ACK) {
            remove_acked_segments(kcp, sn, una);
        } else if (cmd == IKCP_CMD_PUSH) {
            append_ack(kcp, sn, ts);
            if (timediff(sn, kcp->rcv_nxt) >= 0) {
                segment = (kcp_segment_t *) g_malloc_fn(sizeof(*segment));
                if (segment == NULL) {
                    return -2;
                }
                memset(segment, 0, sizeof(*segment));
                segment->data = (char *) g_malloc_fn((size_t) len);
                if (segment->data == NULL) {
                    g_free_fn(segment);
                    return -2;
                }
                memcpy(segment->data, data, (size_t) len);
                segment->len = len;
                segment->sn = sn;
                segment->ts = ts;
                insert_rcv_buf(kcp, segment);
            }
            remove_acked_segments(kcp, sn, una);
        }

        data += len;
        size -= (long) len;
    }

    return 0;
}

int ikcp_peeksize(const ikcpcb *kcp)
{
    return kcp != NULL && kcp->rcv_queue_head != NULL ? (int) kcp->rcv_queue_head->len : -1;
}

int ikcp_setmtu(ikcpcb *kcp, int mtu)
{
    if (kcp == NULL || mtu < (int) IKCP_OVERHEAD + 1) {
        return -1;
    }

    kcp->mtu = (IUINT32) mtu;
    kcp->mss = (IUINT32) mtu - IKCP_OVERHEAD;
    return 0;
}

int ikcp_wndsize(ikcpcb *kcp, int sndwnd, int rcvwnd)
{
    (void) kcp;
    (void) sndwnd;
    (void) rcvwnd;
    return 0;
}

int ikcp_waitsnd(const ikcpcb *kcp)
{
    int count = 0;
    const kcp_segment_t *segment;

    if (kcp == NULL) {
        return 0;
    }

    for (segment = kcp->snd_queue_head; segment != NULL; segment = segment->next) {
        ++count;
    }
    for (segment = kcp->snd_buf_head; segment != NULL; segment = segment->next) {
        ++count;
    }

    return count;
}

int ikcp_nodelay(ikcpcb *kcp, int nodelay, int interval, int resend, int nc)
{
    (void) nodelay;
    (void) resend;
    (void) nc;

    if (kcp == NULL) {
        return -1;
    }

    if (interval >= 10) {
        kcp->interval = (IUINT32) interval;
    }

    return 0;
}

void ikcp_log(ikcpcb *kcp, int mask, const char *fmt, ...)
{
    va_list args;

    (void) kcp;
    (void) mask;
    (void) fmt;
    va_start(args, fmt);
    va_end(args);
}

void ikcp_allocator(void *(*new_malloc)(size_t), void (*new_free)(void *))
{
    g_malloc_fn = new_malloc != NULL ? new_malloc : malloc;
    g_free_fn = new_free != NULL ? new_free : free;
}

IUINT32 ikcp_getconv(const void *ptr)
{
    IUINT32 conv = 0;

    if (ptr != NULL) {
        decode32u((const char *) ptr, &conv);
    }

    return conv;
}
