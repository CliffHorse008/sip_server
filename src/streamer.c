#include "sipserver/streamer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "internal/ikcp.h"
#include "internal/net.h"

#define RTP_HEADER_SIZE 12
#define RTP_MAX_PAYLOAD 1200
#define RTP_CLOCK_VIDEO 90000U
#define STREAMER_QUEUE_DEPTH 128
#define DEFAULT_AAC_SAMPLE_RATE 48000
#define DEFAULT_AAC_CHANNELS 2
#define DEFAULT_AAC_CONFIG_HEX "1190"
#define DEFAULT_H264_PROFILE_LEVEL_ID "42E01F"
#define KCP_AUDIO_CONV 0x41554430U
#define KCP_VIDEO_CONV 0x56494430U
#define KCP_WAITSND_WARN_THRESHOLD 256
#define KCP_WAITSND_WARN_CLEAR_THRESHOLD 128
#define KCP_WAITSND_WARN_INTERVAL_MS 1000U

typedef struct media_transport_state media_transport_state_t;

typedef struct {
    streamer_t *streamer;
    streamer_media_kind_t kind;
    media_transport_state_t *transport_state;
} receive_thread_args_t;

struct streamer {
    const app_config_t *config;
    volatile int stop_requested;
    pthread_t audio_thread;
    pthread_t video_thread;
    pthread_t audio_receive_thread;
    pthread_t video_receive_thread;
    int audio_running;
    int video_running;
    int audio_receive_running;
    int video_receive_running;
    int audio_socket_fd;
    int video_socket_fd;
    receive_thread_args_t audio_receive_args;
    receive_thread_args_t video_receive_args;
    streamer_session_params_t session;
    streamer_receive_callback_t receive_callback;
    void *receive_user_data;
    struct media_frame_queue *audio_queue_ptr;
    struct media_frame_queue *video_queue_ptr;
    media_transport_state_t *audio_transport_state_ptr;
    media_transport_state_t *video_transport_state_ptr;
};

typedef struct {
    media_transport_state_t *transport_state;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_context_t;

typedef struct media_frame {
    unsigned char *data;
    size_t size;
    uint32_t timestamp;
    struct media_frame *next;
} media_frame_t;

typedef struct media_frame_queue {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    media_frame_t *head;
    media_frame_t *tail;
    size_t depth;
    size_t max_depth;
    int stopped;
} media_frame_queue_t;

struct media_transport_state {
    int socket_fd;
    int use_kcp;
    int has_remote;
    struct sockaddr_in remote_addr;
    int has_alt_remote;
    struct sockaddr_in alt_remote_addr;
    uint32_t kcp_conv;
    ikcpcb *kcp;
    pthread_mutex_t mutex;
    char label[16];
    int kcp_waitsnd_high;
    uint32_t kcp_waitsnd_last_warn_ms;
};

void streamer_stop(streamer_t *streamer);

static unsigned int random_u32(void)
{
    return ((unsigned int) rand() << 16) ^ (unsigned int) rand();
}

static uint32_t monotonic_time_ms32(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint32_t) ((uint64_t) tv.tv_sec * 1000ULL + (uint64_t) tv.tv_usec / 1000ULL);
}

static int string_case_prefix(const char *text, const char *prefix)
{
    while (*prefix != '\0') {
        unsigned char lhs;
        unsigned char rhs;

        if (*text == '\0') {
            return 0;
        }

        lhs = (unsigned char) *text++;
        rhs = (unsigned char) *prefix++;
        if (lhs >= 'A' && lhs <= 'Z') {
            lhs = (unsigned char) (lhs - 'A' + 'a');
        }
        if (rhs >= 'A' && rhs <= 'Z') {
            rhs = (unsigned char) (rhs - 'A' + 'a');
        }
        if (lhs != rhs) {
            return 0;
        }
    }

    return 1;
}

static int transport_uses_kcp(const char *transport)
{
    return transport != NULL && string_case_prefix(transport, "kcp/");
}

static const char *default_media_transport(const app_config_t *config)
{
    return config->rtp_transport == RTP_TRANSPORT_KCP ? "KCP/RTP/AVP" : "RTP/AVP";
}

static int transport_send_raw_locked(media_transport_state_t *state, const unsigned char *packet, size_t packet_size)
{
    if (!state->has_remote) {
        errno = ENOTCONN;
        return -1;
    }

    if (sendto(state->socket_fd,
               packet,
               packet_size,
               0,
               (const struct sockaddr *) &state->remote_addr,
               sizeof(state->remote_addr)) < 0) {
        return -1;
    }

    if (state->has_alt_remote) {
        if (sendto(state->socket_fd,
                   packet,
                   packet_size,
                   0,
                   (const struct sockaddr *) &state->alt_remote_addr,
                   sizeof(state->alt_remote_addr)) < 0) {
            return -1;
        }
    }

    return 0;
}

static int kcp_output_callback(const char *buf, int len, ikcpcb *kcp, void *user)
{
    media_transport_state_t *state = (media_transport_state_t *) user;

    (void) kcp;
    return transport_send_raw_locked(state, (const unsigned char *) buf, (size_t) len);
}

static int media_transport_create_kcp_locked(media_transport_state_t *state)
{
    ikcpcb *kcp;

    if (state == NULL || !state->use_kcp) {
        return 0;
    }

    kcp = ikcp_create(state->kcp_conv, state);
    if (kcp == NULL) {
        return -1;
    }

    ikcp_setoutput(kcp, kcp_output_callback);
    ikcp_setmtu(kcp, 1400);
    ikcp_wndsize(kcp, 256, 256);
    ikcp_nodelay(kcp, 1, 20, 2, 1);
    state->kcp = kcp;
    state->kcp_waitsnd_high = 0;
    state->kcp_waitsnd_last_warn_ms = 0;
    return 0;
}

static int media_transport_update_kcp_backlog_locked(media_transport_state_t *state)
{
    int waitsnd;
    uint32_t now_ms;

    if (state == NULL || !state->use_kcp || state->kcp == NULL) {
        return 0;
    }

    ikcp_update(state->kcp, monotonic_time_ms32());
    waitsnd = ikcp_waitsnd(state->kcp);
    now_ms = monotonic_time_ms32();
    if (waitsnd > KCP_WAITSND_WARN_THRESHOLD) {
        if (!state->kcp_waitsnd_high ||
            (uint32_t) (now_ms - state->kcp_waitsnd_last_warn_ms) >= KCP_WAITSND_WARN_INTERVAL_MS) {
            char remote_ip[64];

            sockaddr_to_ip_string(&state->remote_addr, remote_ip, sizeof(remote_ip));
            fprintf(stdout,
                    "kcp %s backlog high waitsnd=%d threshold=%d conv=0x%08x remote=%s:%u\n",
                    state->label[0] != '\0' ? state->label : "media",
                    waitsnd,
                    KCP_WAITSND_WARN_THRESHOLD,
                    state->kcp_conv,
                    remote_ip,
                    ntohs(state->remote_addr.sin_port));
            state->kcp_waitsnd_last_warn_ms = now_ms;
        }
        state->kcp_waitsnd_high = 1;
    } else if (state->kcp_waitsnd_high && waitsnd <= KCP_WAITSND_WARN_CLEAR_THRESHOLD) {
        fprintf(stdout,
                "kcp %s backlog recovered waitsnd=%d clear_threshold=%d conv=0x%08x\n",
                state->label[0] != '\0' ? state->label : "media",
                waitsnd,
                KCP_WAITSND_WARN_CLEAR_THRESHOLD,
                state->kcp_conv);
        state->kcp_waitsnd_high = 0;
        state->kcp_waitsnd_last_warn_ms = 0;
    }

    return waitsnd > KCP_WAITSND_WARN_THRESHOLD;
}

static int media_transport_state_init(media_transport_state_t *state, int socket_fd, const char *label)
{
    memset(state, 0, sizeof(*state));
    state->socket_fd = socket_fd;
    if (label != NULL) {
        snprintf(state->label, sizeof(state->label), "%s", label);
    }
    return pthread_mutex_init(&state->mutex, NULL);
}

static void media_transport_state_reset(media_transport_state_t *state)
{
    if (state == NULL) {
        return;
    }

    pthread_mutex_lock(&state->mutex);
    if (state->kcp != NULL) {
        ikcp_release(state->kcp);
        state->kcp = NULL;
    }
    state->use_kcp = 0;
    state->has_remote = 0;
    state->has_alt_remote = 0;
    state->kcp_conv = 0;
    state->kcp_waitsnd_high = 0;
    state->kcp_waitsnd_last_warn_ms = 0;
    memset(&state->remote_addr, 0, sizeof(state->remote_addr));
    memset(&state->alt_remote_addr, 0, sizeof(state->alt_remote_addr));
    pthread_mutex_unlock(&state->mutex);
}

static void media_transport_state_destroy(media_transport_state_t *state)
{
    if (state == NULL) {
        return;
    }

    media_transport_state_reset(state);
    pthread_mutex_destroy(&state->mutex);
}

static int media_transport_state_configure(media_transport_state_t *state,
                                           const char *transport,
                                           const char *remote_ip,
                                           const char *remote_ip_alt,
                                           uint16_t remote_port,
                                           uint32_t kcp_conv)
{
    if (state == NULL || remote_ip == NULL || remote_ip[0] == '\0' || remote_port == 0) {
        return -1;
    }

    pthread_mutex_lock(&state->mutex);
    if (state->kcp != NULL) {
        ikcp_release(state->kcp);
        state->kcp = NULL;
    }
    state->use_kcp = transport_uses_kcp(transport);
    state->has_remote = 0;
    state->has_alt_remote = 0;
    state->kcp_conv = state->use_kcp ? kcp_conv : 0;

    if (sockaddr_from_ip_port(remote_ip, remote_port, &state->remote_addr) != 0) {
        pthread_mutex_unlock(&state->mutex);
        return -1;
    }
    state->has_remote = 1;

    if (remote_ip_alt != NULL &&
        remote_ip_alt[0] != '\0' &&
        strcmp(remote_ip_alt, remote_ip) != 0 &&
        sockaddr_from_ip_port(remote_ip_alt, remote_port, &state->alt_remote_addr) == 0) {
        state->has_alt_remote = 1;
    }

    if (!state->use_kcp) {
        pthread_mutex_unlock(&state->mutex);
        return 0;
    }

    if (media_transport_create_kcp_locked(state) != 0) {
        state->has_remote = 0;
        state->has_alt_remote = 0;
        pthread_mutex_unlock(&state->mutex);
        return -1;
    }
    pthread_mutex_unlock(&state->mutex);
    return 0;
}

static void media_transport_tick(media_transport_state_t *state)
{
    if (state == NULL) {
        return;
    }

    pthread_mutex_lock(&state->mutex);
    if (state->use_kcp && state->kcp != NULL) {
        media_transport_update_kcp_backlog_locked(state);
    }
    pthread_mutex_unlock(&state->mutex);
}

static int media_transport_send_packet(media_transport_state_t *state,
                                       const unsigned char *packet,
                                       size_t packet_size)
{
    int rc;

    if (state == NULL) {
        errno = ENOTCONN;
        return -1;
    }

    pthread_mutex_lock(&state->mutex);
    if (!state->use_kcp) {
        rc = transport_send_raw_locked(state, packet, packet_size);
        pthread_mutex_unlock(&state->mutex);
        return rc;
    }

    if (state->kcp == NULL) {
        pthread_mutex_unlock(&state->mutex);
        errno = ENOTCONN;
        return -1;
    }

    ikcp_update(state->kcp, monotonic_time_ms32());
    rc = ikcp_send(state->kcp, (const char *) packet, (int) packet_size);
    if (rc >= 0) {
        ikcp_flush(state->kcp);
        media_transport_update_kcp_backlog_locked(state);
        rc = 0;
    } else {
        rc = -1;
    }
    pthread_mutex_unlock(&state->mutex);
    return rc;
}

static int set_socket_receive_timeout(int socket_fd, int timeout_ms)
{
    struct timeval timeout;

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

static int find_start_code(const unsigned char *data, size_t size, size_t offset, size_t *index, size_t *prefix_len)
{
    size_t i;

    for (i = offset; i + 3 < size; ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                *index = i;
                *prefix_len = 3;
                return 1;
            }
            if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                *index = i;
                *prefix_len = 4;
                return 1;
            }
        }
    }

    return 0;
}

static int create_media_socket(const char *bind_ip, uint16_t port)
{
    int socket_fd = udp_socket_bind(bind_ip, port);

    if (socket_fd < 0) {
        return -1;
    }

    if (set_socket_receive_timeout(socket_fd, 1000) != 0) {
        perror("setsockopt(SO_RCVTIMEO)");
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static media_frame_queue_t *media_queue_create(size_t max_depth)
{
    media_frame_queue_t *queue = (media_frame_queue_t *) calloc(1, sizeof(*queue));

    if (queue == NULL) {
        return NULL;
    }

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
    queue->max_depth = max_depth;
    return queue;
}

static void media_queue_drop_all(media_frame_queue_t *queue)
{
    media_frame_t *node;
    media_frame_t *next;

    if (queue == NULL) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    node = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    queue->depth = 0;
    pthread_mutex_unlock(&queue->mutex);

    while (node != NULL) {
        next = node->next;
        free(node->data);
        free(node);
        node = next;
    }
}

static void media_queue_wake_and_stop(media_frame_queue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->stopped = 1;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

static void media_queue_reset(media_frame_queue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->stopped = 0;
    pthread_mutex_unlock(&queue->mutex);
    media_queue_drop_all(queue);
}

static void media_queue_destroy(media_frame_queue_t *queue)
{
    if (queue == NULL) {
        return;
    }

    media_queue_wake_and_stop(queue);
    media_queue_reset(queue);
    pthread_cond_destroy(&queue->cond);
    pthread_mutex_destroy(&queue->mutex);
    free(queue);
}

static int media_queue_push(media_frame_queue_t *queue, const uint8_t *data, size_t size, uint32_t timestamp)
{
    media_frame_t *node;

    if (queue == NULL || data == NULL || size == 0) {
        return -1;
    }

    node = (media_frame_t *) calloc(1, sizeof(*node));
    if (node == NULL) {
        return -1;
    }
    node->data = (unsigned char *) malloc(size);
    if (node->data == NULL) {
        free(node);
        return -1;
    }

    memcpy(node->data, data, size);
    node->size = size;
    node->timestamp = timestamp;

    pthread_mutex_lock(&queue->mutex);
    if (queue->stopped) {
        pthread_mutex_unlock(&queue->mutex);
        free(node->data);
        free(node);
        return -1;
    }

    if (queue->depth >= queue->max_depth && queue->head != NULL) {
        pthread_mutex_unlock(&queue->mutex);
        free(node->data);
        free(node);
        return -1;
    }

    if (queue->tail == NULL) {
        queue->head = node;
        queue->tail = node;
    } else {
        queue->tail->next = node;
        queue->tail = node;
    }
    ++queue->depth;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
    return 0;
}

static int media_queue_pop(media_frame_queue_t *queue, media_frame_t *frame, int wait_ms)
{
    int rc = 0;

    memset(frame, 0, sizeof(*frame));
    pthread_mutex_lock(&queue->mutex);
    while (queue->head == NULL && !queue->stopped) {
        if (wait_ms <= 0) {
            pthread_mutex_unlock(&queue->mutex);
            return 0;
        }

        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += wait_ms / 1000;
            ts.tv_nsec += (long) (wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            rc = pthread_cond_timedwait(&queue->cond, &queue->mutex, &ts);
        }

        if (rc != 0) {
            pthread_mutex_unlock(&queue->mutex);
            return 0;
        }
    }

    if (queue->stopped || queue->head == NULL) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    *frame = *queue->head;
    queue->head = frame->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    frame->next = NULL;
    --queue->depth;
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

void streamer_set_receive_callback(streamer_t *streamer,
                                   streamer_receive_callback_t callback,
                                   void *user_data)
{
    if (streamer == NULL) {
        return;
    }

    streamer->receive_callback = callback;
    streamer->receive_user_data = user_data;
}

int streamer_push_audio_frame(streamer_t *streamer, const streamer_audio_frame_t *frame)
{
    if (streamer == NULL || frame == NULL) {
        return -1;
    }

    return media_queue_push(streamer->audio_queue_ptr, frame->data, frame->size, frame->timestamp);
}

int streamer_push_video_frame(streamer_t *streamer, const streamer_video_frame_t *frame)
{
    if (streamer == NULL || frame == NULL) {
        return -1;
    }

    return media_queue_push(streamer->video_queue_ptr, frame->data, frame->size, frame->timestamp);
}

int streamer_audio_backpressure_high(streamer_t *streamer)
{
    int high = 0;

    if (streamer == NULL || streamer->audio_transport_state_ptr == NULL) {
        return 0;
    }

    pthread_mutex_lock(&streamer->audio_transport_state_ptr->mutex);
    high = media_transport_update_kcp_backlog_locked(streamer->audio_transport_state_ptr);
    pthread_mutex_unlock(&streamer->audio_transport_state_ptr->mutex);
    return high;
}

int streamer_video_backpressure_high(streamer_t *streamer)
{
    int high = 0;

    if (streamer == NULL || streamer->video_transport_state_ptr == NULL) {
        return 0;
    }

    pthread_mutex_lock(&streamer->video_transport_state_ptr->mutex);
    high = media_transport_update_kcp_backlog_locked(streamer->video_transport_state_ptr);
    pthread_mutex_unlock(&streamer->video_transport_state_ptr->mutex);
    return high;
}

static int rtp_send_packet(rtp_context_t *context,
                           uint8_t payload_type,
                           int marker,
                           const unsigned char *payload,
                           size_t payload_size)
{
    unsigned char packet[RTP_HEADER_SIZE + RTP_MAX_PAYLOAD];

    if (payload_size > RTP_MAX_PAYLOAD) {
        return -1;
    }

    packet[0] = 0x80;
    packet[1] = (uint8_t) ((marker ? 0x80 : 0x00) | (payload_type & 0x7F));
    packet[2] = (unsigned char) (context->sequence >> 8);
    packet[3] = (unsigned char) (context->sequence & 0xFF);
    packet[4] = (unsigned char) (context->timestamp >> 24);
    packet[5] = (unsigned char) (context->timestamp >> 16);
    packet[6] = (unsigned char) (context->timestamp >> 8);
    packet[7] = (unsigned char) (context->timestamp & 0xFF);
    packet[8] = (unsigned char) (context->ssrc >> 24);
    packet[9] = (unsigned char) (context->ssrc >> 16);
    packet[10] = (unsigned char) (context->ssrc >> 8);
    packet[11] = (unsigned char) (context->ssrc & 0xFF);
    memcpy(packet + RTP_HEADER_SIZE, payload, payload_size);

    if (media_transport_send_packet(context->transport_state,
                                    packet,
                                    RTP_HEADER_SIZE + payload_size) != 0) {
        return -1;
    }

    ++context->sequence;
    return 0;
}

static int rtp_send_h264_nal(rtp_context_t *context,
                             uint8_t payload_type,
                             const unsigned char *nal,
                             size_t nal_length,
                             int marker)
{
    unsigned char payload[RTP_MAX_PAYLOAD];

    if (nal_length <= RTP_MAX_PAYLOAD) {
        return rtp_send_packet(context, payload_type, marker, nal, nal_length);
    }

    if (nal_length < 2) {
        return -1;
    }

    {
        unsigned char nal_header = nal[0];
        unsigned char fu_indicator = (unsigned char) ((nal_header & 0xE0) | 28U);
        unsigned char fu_header = (unsigned char) (nal_header & 0x1F);
        size_t offset = 1;

        while (offset < nal_length) {
            size_t chunk = nal_length - offset;
            int start = offset == 1;
            int end;

            if (chunk > RTP_MAX_PAYLOAD - 2) {
                chunk = RTP_MAX_PAYLOAD - 2;
            }
            end = offset + chunk >= nal_length;

            payload[0] = fu_indicator;
            payload[1] = (unsigned char) (fu_header |
                                          (start ? 0x80U : 0x00U) |
                                          (end ? 0x40U : 0x00U));
            memcpy(payload + 2, nal + offset, chunk);

            if (rtp_send_packet(context, payload_type, marker && end, payload, chunk + 2) != 0) {
                return -1;
            }

            offset += chunk;
        }
    }

    return 0;
}

static int rtp_send_h264_access_unit(rtp_context_t *context,
                                     uint8_t payload_type,
                                     const unsigned char *data,
                                     size_t size)
{
    size_t offset = 0;
    size_t start;
    size_t prefix_len;
    int sent = 0;

    if (find_start_code(data, size, 0, &start, &prefix_len)) {
        while (1) {
            size_t nal_offset = start + prefix_len;
            size_t next_start;
            size_t next_prefix_len;
            size_t nal_end;
            int has_next;

            has_next = find_start_code(data, size, nal_offset, &next_start, &next_prefix_len);
            nal_end = has_next ? next_start : size;
            while (nal_end > nal_offset && data[nal_end - 1] == 0x00) {
                --nal_end;
            }

            if (nal_end > nal_offset) {
                int marker = !has_next;

                if (rtp_send_h264_nal(context, payload_type, data + nal_offset, nal_end - nal_offset, marker) != 0) {
                    return -1;
                }
                sent = 1;
            }

            if (!has_next) {
                break;
            }

            offset = next_start;
            start = next_start;
            prefix_len = next_prefix_len;
            (void) offset;
        }

        return sent ? 0 : -1;
    }

    return rtp_send_h264_nal(context, payload_type, data, size, 1);
}

static int rtp_send_aac_frame(rtp_context_t *context,
                              uint8_t payload_type,
                              const unsigned char *frame,
                              size_t frame_length)
{
    unsigned char payload[RTP_MAX_PAYLOAD];

    if (frame_length + 4 > RTP_MAX_PAYLOAD) {
        return -1;
    }

    payload[0] = 0x00;
    payload[1] = 0x10;
    payload[2] = (unsigned char) ((frame_length >> 5) & 0xFF);
    payload[3] = (unsigned char) ((frame_length & 0x1F) << 3);
    memcpy(payload + 4, frame, frame_length);

    return rtp_send_packet(context, payload_type, 1, payload, frame_length + 4);
}

static int rtp_send_g711a_frame(rtp_context_t *context,
                                uint8_t payload_type,
                                const unsigned char *frame,
                                size_t frame_length)
{
    return rtp_send_packet(context, payload_type, 1, frame, frame_length);
}

static void media_frame_release(media_frame_t *frame)
{
    free(frame->data);
    memset(frame, 0, sizeof(*frame));
}

static int create_rtp_context(const app_config_t *config,
                              media_transport_state_t *transport_state,
                              const char *transport,
                              uint16_t local_port,
                              const char *remote_ip,
                              const char *remote_ip_alt,
                              uint16_t remote_port,
                              uint32_t initial_timestamp,
                              rtp_context_t *context)
{
    (void) config;
    (void) transport;
    (void) local_port;
    (void) remote_ip;
    (void) remote_ip_alt;
    (void) remote_port;
    memset(context, 0, sizeof(*context));
    if (transport_state == NULL) {
        return -1;
    }
    context->transport_state = transport_state;
    context->sequence = (uint16_t) random_u32();
    context->timestamp = initial_timestamp;
    context->ssrc = random_u32();
    return 0;
}

static void destroy_rtp_context(rtp_context_t *context)
{
    context->transport_state = NULL;
}

static size_t rtp_header_size(const unsigned char *packet, size_t packet_size)
{
    size_t csrc_count;
    size_t header_size;
    int extension;

    if (packet_size < RTP_HEADER_SIZE) {
        return 0;
    }

    csrc_count = (size_t) (packet[0] & 0x0F);
    extension = (packet[0] & 0x10) != 0;
    header_size = RTP_HEADER_SIZE + csrc_count * 4U;

    if (packet_size < header_size) {
        return 0;
    }

    if (extension) {
        size_t extension_words;

        if (packet_size < header_size + 4U) {
            return 0;
        }

        extension_words = ((size_t) packet[header_size + 2] << 8) | (size_t) packet[header_size + 3];
        header_size += 4U + extension_words * 4U;
        if (packet_size < header_size) {
            return 0;
        }
    }

    return header_size;
}

static void deliver_received_rtp(streamer_t *streamer,
                                 streamer_media_kind_t kind,
                                 const unsigned char *packet,
                                 size_t packet_size,
                                 const struct sockaddr_in *peer)
{
    char source_ip[64];
    streamer_rtp_packet_t callback_packet;
    size_t header_size;

    if (streamer->receive_callback == NULL) {
        return;
    }

    header_size = rtp_header_size(packet, packet_size);
    if (header_size == 0 || packet_size < header_size) {
        return;
    }

    sockaddr_to_ip_string(peer, source_ip, sizeof(source_ip));

    callback_packet.kind = kind;
    callback_packet.payload = packet + header_size;
    callback_packet.payload_size = packet_size - header_size;
    callback_packet.payload_type = (uint8_t) (packet[1] & 0x7F);
    callback_packet.sequence = (uint16_t) (((uint16_t) packet[2] << 8) | (uint16_t) packet[3]);
    callback_packet.timestamp = ((uint32_t) packet[4] << 24) |
                                ((uint32_t) packet[5] << 16) |
                                ((uint32_t) packet[6] << 8) |
                                (uint32_t) packet[7];
    callback_packet.ssrc = ((uint32_t) packet[8] << 24) |
                           ((uint32_t) packet[9] << 16) |
                           ((uint32_t) packet[10] << 8) |
                           (uint32_t) packet[11];
    callback_packet.source_ip = source_ip;
    callback_packet.source_port = ntohs(peer->sin_port);

    streamer->receive_callback(&callback_packet, streamer->receive_user_data);
}

static void media_transport_receive(streamer_t *streamer,
                                    media_transport_state_t *state,
                                    streamer_media_kind_t kind,
                                    const unsigned char *packet,
                                    size_t packet_size,
                                    const struct sockaddr_in *peer)
{
    unsigned char decoded[1600];

    if (state == NULL) {
        return;
    }

    if (!state->use_kcp) {
        deliver_received_rtp(streamer, kind, packet, packet_size, peer);
        return;
    }

    pthread_mutex_lock(&state->mutex);
    if (state->kcp == NULL) {
        pthread_mutex_unlock(&state->mutex);
        return;
    }

    ikcp_update(state->kcp, monotonic_time_ms32());
    if (ikcp_input(state->kcp, (const char *) packet, (long) packet_size) != 0) {
        pthread_mutex_unlock(&state->mutex);
        return;
    }
    ikcp_flush(state->kcp);

    while (1) {
        int packet_len = ikcp_peeksize(state->kcp);

        if (packet_len <= 0 || packet_len > (int) sizeof(decoded)) {
            break;
        }

        packet_len = ikcp_recv(state->kcp, (char *) decoded, (int) sizeof(decoded));
        if (packet_len <= 0) {
            break;
        }

        pthread_mutex_unlock(&state->mutex);
        deliver_received_rtp(streamer, kind, decoded, (size_t) packet_len, peer);
        pthread_mutex_lock(&state->mutex);
        if (state->kcp == NULL) {
            break;
        }
    }

    pthread_mutex_unlock(&state->mutex);
}

static void *receive_thread_main(void *opaque)
{
    receive_thread_args_t *args = (receive_thread_args_t *) opaque;
    streamer_t *streamer = args->streamer;
    unsigned char packet[1600];

    while (!streamer->stop_requested) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t received = recvfrom(args->transport_state->socket_fd,
                                    packet,
                                    sizeof(packet),
                                    0,
                                    (struct sockaddr *) &peer,
                                    &peer_len);

        if (received < 0) {
            continue;
        }

        media_transport_receive(streamer,
                                args->transport_state,
                                args->kind,
                                packet,
                                (size_t) received,
                                &peer);
    }

    return NULL;
}

static void *video_thread_main(void *opaque)
{
    streamer_t *streamer = (streamer_t *) opaque;
    rtp_context_t context;

    if (create_rtp_context(streamer->config,
                           streamer->video_transport_state_ptr,
                           streamer->session.video_transport,
                           streamer->config->video_port,
                           streamer->session.remote_ip,
                           streamer->session.remote_ip_alt,
                           streamer->session.video_port,
                           random_u32(),
                           &context) != 0) {
        return NULL;
    }

    fprintf(stdout,
            "video RTP start local=%u remote=%s%s%s:%u pt=%u transport=%s\n",
            streamer->config->video_port,
            streamer->session.remote_ip,
            streamer->session.remote_ip_alt[0] != '\0' ? " alt=" : "",
            streamer->session.remote_ip_alt[0] != '\0' ? streamer->session.remote_ip_alt : "",
            streamer->session.video_port,
            streamer->session.video_payload_type,
            streamer->session.video_transport);

    while (!streamer->stop_requested) {
        media_frame_t frame;
        media_transport_tick(streamer->video_transport_state_ptr);

        if (media_queue_pop(streamer->video_queue_ptr, &frame, 10)) {
            context.timestamp = frame.timestamp;
            if (rtp_send_h264_access_unit(&context,
                                          streamer->session.video_payload_type,
                                          frame.data,
                                          frame.size) != 0) {
                perror("send(video-live)");
                media_frame_release(&frame);
                break;
            }
            media_frame_release(&frame);

            continue;
        }
    }

    destroy_rtp_context(&context);
    return NULL;
}

static void *audio_thread_main(void *opaque)
{
    streamer_t *streamer = (streamer_t *) opaque;
    rtp_context_t context;

    if (create_rtp_context(streamer->config,
                           streamer->audio_transport_state_ptr,
                           streamer->session.audio_transport,
                           streamer->config->audio_port,
                           streamer->session.remote_ip,
                           streamer->session.remote_ip_alt,
                           streamer->session.audio_port,
                           random_u32(),
                           &context) != 0) {
        return NULL;
    }

    fprintf(stdout,
            "audio RTP start local=%u remote=%s%s%s:%u pt=%u codec=%s transport=%s\n",
            streamer->config->audio_port,
            streamer->session.remote_ip,
            streamer->session.remote_ip_alt[0] != '\0' ? " alt=" : "",
            streamer->session.remote_ip_alt[0] != '\0' ? streamer->session.remote_ip_alt : "",
            streamer->session.audio_port,
            streamer->session.audio_payload_type,
            config_audio_codec_name(streamer->session.audio_codec),
            streamer->session.audio_transport);

    if (streamer->session.audio_codec == AUDIO_CODEC_AAC) {
        while (!streamer->stop_requested) {
            media_frame_t frame;
            media_transport_tick(streamer->audio_transport_state_ptr);

            if (media_queue_pop(streamer->audio_queue_ptr, &frame, 10)) {
                context.timestamp = frame.timestamp;
                if (rtp_send_aac_frame(&context,
                                       streamer->session.audio_payload_type,
                                       frame.data,
                                       frame.size) != 0) {
                    perror("send(audio-aac-live)");
                    media_frame_release(&frame);
                    break;
                }
                media_frame_release(&frame);
                continue;
            }
        }
    } else {
        while (!streamer->stop_requested) {
            media_frame_t frame;
            media_transport_tick(streamer->audio_transport_state_ptr);

            if (media_queue_pop(streamer->audio_queue_ptr, &frame, 10)) {
                context.timestamp = frame.timestamp;
                if (rtp_send_g711a_frame(&context,
                                         streamer->session.audio_payload_type,
                                         frame.data,
                                         frame.size) != 0) {
                    perror("send(audio-g711a-live)");
                    media_frame_release(&frame);
                    break;
                }
                media_frame_release(&frame);
                continue;
            }
        }
    }

    destroy_rtp_context(&context);
    return NULL;
}

streamer_t *streamer_create(const app_config_t *config)
{
    streamer_t *streamer = (streamer_t *) calloc(1, sizeof(*streamer));

    if (streamer == NULL) {
        return NULL;
    }

    streamer->config = config;
    streamer->audio_socket_fd = -1;
    streamer->video_socket_fd = -1;
    streamer->audio_transport_state_ptr = NULL;
    streamer->video_transport_state_ptr = NULL;
    streamer->audio_queue_ptr = media_queue_create(STREAMER_QUEUE_DEPTH);
    streamer->video_queue_ptr = media_queue_create(STREAMER_QUEUE_DEPTH);
    if (streamer->audio_queue_ptr == NULL || streamer->video_queue_ptr == NULL) {
        media_queue_destroy(streamer->audio_queue_ptr);
        media_queue_destroy(streamer->video_queue_ptr);
        free(streamer);
        return NULL;
    }

    streamer->audio_socket_fd = create_media_socket(config->bind_ip, config->audio_port);
    if (streamer->audio_socket_fd < 0) {
        media_queue_destroy(streamer->audio_queue_ptr);
        media_queue_destroy(streamer->video_queue_ptr);
        free(streamer);
        return NULL;
    }

    streamer->video_socket_fd = create_media_socket(config->bind_ip, config->video_port);
    if (streamer->video_socket_fd < 0) {
        close(streamer->audio_socket_fd);
        media_queue_destroy(streamer->audio_queue_ptr);
        media_queue_destroy(streamer->video_queue_ptr);
        free(streamer);
        return NULL;
    }

    streamer->audio_transport_state_ptr = (media_transport_state_t *) calloc(1, sizeof(*streamer->audio_transport_state_ptr));
    streamer->video_transport_state_ptr = (media_transport_state_t *) calloc(1, sizeof(*streamer->video_transport_state_ptr));
    if (streamer->audio_transport_state_ptr == NULL || streamer->video_transport_state_ptr == NULL) {
        free(streamer->audio_transport_state_ptr);
        free(streamer->video_transport_state_ptr);
        close(streamer->audio_socket_fd);
        close(streamer->video_socket_fd);
        media_queue_destroy(streamer->audio_queue_ptr);
        media_queue_destroy(streamer->video_queue_ptr);
        free(streamer);
        return NULL;
    }

    {
        int audio_state_ready = media_transport_state_init(streamer->audio_transport_state_ptr,
                                                           streamer->audio_socket_fd,
                                                           "audio") == 0;
        int video_state_ready = media_transport_state_init(streamer->video_transport_state_ptr,
                                                           streamer->video_socket_fd,
                                                           "video") == 0;

        if (!audio_state_ready || !video_state_ready) {
            if (audio_state_ready) {
                pthread_mutex_destroy(&streamer->audio_transport_state_ptr->mutex);
            }
            if (video_state_ready) {
                pthread_mutex_destroy(&streamer->video_transport_state_ptr->mutex);
            }
            free(streamer->audio_transport_state_ptr);
            free(streamer->video_transport_state_ptr);
            close(streamer->audio_socket_fd);
            close(streamer->video_socket_fd);
            media_queue_destroy(streamer->audio_queue_ptr);
            media_queue_destroy(streamer->video_queue_ptr);
            free(streamer);
            return NULL;
        }
    }

    return streamer;
}

void streamer_destroy(streamer_t *streamer)
{
    if (streamer == NULL) {
        return;
    }

    streamer_stop(streamer);
    if (streamer->audio_socket_fd >= 0) {
        close(streamer->audio_socket_fd);
    }
    if (streamer->video_socket_fd >= 0) {
        close(streamer->video_socket_fd);
    }
    if (streamer->audio_transport_state_ptr != NULL) {
        media_transport_state_destroy(streamer->audio_transport_state_ptr);
        free(streamer->audio_transport_state_ptr);
    }
    if (streamer->video_transport_state_ptr != NULL) {
        media_transport_state_destroy(streamer->video_transport_state_ptr);
        free(streamer->video_transport_state_ptr);
    }
    media_queue_destroy(streamer->audio_queue_ptr);
    media_queue_destroy(streamer->video_queue_ptr);
    free(streamer);
}

void streamer_stop(streamer_t *streamer)
{
    streamer->stop_requested = 1;
    media_queue_wake_and_stop(streamer->audio_queue_ptr);
    media_queue_wake_and_stop(streamer->video_queue_ptr);

    if (streamer->video_running) {
        pthread_join(streamer->video_thread, NULL);
        streamer->video_running = 0;
    }
    if (streamer->audio_running) {
        pthread_join(streamer->audio_thread, NULL);
        streamer->audio_running = 0;
    }
    if (streamer->video_receive_running) {
        pthread_join(streamer->video_receive_thread, NULL);
        streamer->video_receive_running = 0;
    }
    if (streamer->audio_receive_running) {
        pthread_join(streamer->audio_receive_thread, NULL);
        streamer->audio_receive_running = 0;
    }

    if (streamer->audio_transport_state_ptr != NULL) {
        media_transport_state_reset(streamer->audio_transport_state_ptr);
    }
    if (streamer->video_transport_state_ptr != NULL) {
        media_transport_state_reset(streamer->video_transport_state_ptr);
    }
}

int streamer_start(streamer_t *streamer, const streamer_session_params_t *params)
{
    if (streamer == NULL || params == NULL) {
        return -1;
    }

    streamer_stop(streamer);
    media_queue_reset(streamer->audio_queue_ptr);
    media_queue_reset(streamer->video_queue_ptr);
    streamer->stop_requested = 0;
    streamer->session = *params;
    if (streamer->session.audio_port != 0 && streamer->session.audio_transport[0] == '\0') {
        snprintf(streamer->session.audio_transport,
                 sizeof(streamer->session.audio_transport),
                 "%s",
                 default_media_transport(streamer->config));
    }
    if (streamer->session.video_port != 0 && streamer->session.video_transport[0] == '\0') {
        snprintf(streamer->session.video_transport,
                 sizeof(streamer->session.video_transport),
                 "%s",
                 default_media_transport(streamer->config));
    }

    if (streamer->session.audio_port != 0 &&
        media_transport_state_configure(streamer->audio_transport_state_ptr,
                                        streamer->session.audio_transport,
                                        streamer->session.remote_ip,
                                        streamer->session.remote_ip_alt,
                                        streamer->session.audio_port,
                                        KCP_AUDIO_CONV) != 0) {
        return -1;
    }

    if (streamer->session.video_enabled &&
        streamer->session.video_port != 0 &&
        media_transport_state_configure(streamer->video_transport_state_ptr,
                                        streamer->session.video_transport,
                                        streamer->session.remote_ip,
                                        streamer->session.remote_ip_alt,
                                        streamer->session.video_port,
                                        KCP_VIDEO_CONV) != 0) {
        media_transport_state_reset(streamer->audio_transport_state_ptr);
        return -1;
    }

    if (streamer->receive_callback != NULL && params->video_enabled && params->video_port != 0) {
        streamer->video_receive_args.streamer = streamer;
        streamer->video_receive_args.kind = STREAMER_MEDIA_VIDEO;
        streamer->video_receive_args.transport_state = streamer->video_transport_state_ptr;
        if (pthread_create(&streamer->video_receive_thread, NULL, receive_thread_main, &streamer->video_receive_args) == 0) {
            streamer->video_receive_running = 1;
        } else {
            perror("pthread_create(video-recv)");
            streamer_stop(streamer);
            return -1;
        }
    }

    if (streamer->receive_callback != NULL && params->audio_port != 0) {
        streamer->audio_receive_args.streamer = streamer;
        streamer->audio_receive_args.kind = STREAMER_MEDIA_AUDIO;
        streamer->audio_receive_args.transport_state = streamer->audio_transport_state_ptr;
        if (pthread_create(&streamer->audio_receive_thread, NULL, receive_thread_main, &streamer->audio_receive_args) == 0) {
            streamer->audio_receive_running = 1;
        } else {
            perror("pthread_create(audio-recv)");
            streamer_stop(streamer);
            return -1;
        }
    }

    if (params->video_enabled && params->video_port != 0) {
        if (pthread_create(&streamer->video_thread, NULL, video_thread_main, streamer) == 0) {
            streamer->video_running = 1;
        } else {
            perror("pthread_create(video)");
            return -1;
        }
    }

    if (params->audio_port != 0) {
        if (pthread_create(&streamer->audio_thread, NULL, audio_thread_main, streamer) == 0) {
            streamer->audio_running = 1;
        } else {
            perror("pthread_create(audio)");
            streamer_stop(streamer);
            return -1;
        }
    }

    fprintf(stdout,
            "media session to %s audio=%u(%s) video=%u(%s) codec=%s\n",
            streamer->session.remote_ip,
            streamer->session.audio_port,
            streamer->session.audio_port != 0 ? streamer->session.audio_transport : "-",
            streamer->session.video_port,
            streamer->session.video_port != 0 ? streamer->session.video_transport : "-",
            config_audio_codec_name(streamer->session.audio_codec));

    return 0;
}

static const char *streamer_direction_name(streamer_direction_t direction)
{
    switch (direction) {
    case STREAMER_DIRECTION_SENDONLY:
        return "sendonly";
    case STREAMER_DIRECTION_RECVONLY:
        return "recvonly";
    case STREAMER_DIRECTION_INACTIVE:
        return "inactive";
    case STREAMER_DIRECTION_SENDRECV:
    default:
        return "sendrecv";
    }
}

int streamer_build_sdp(const streamer_t *streamer,
                       const streamer_sdp_plan_t *plan,
                       char *buffer,
                       size_t buffer_size)
{
    int written = 0;
    size_t index;

    if (plan == NULL) {
        return -1;
    }

    written += snprintf(buffer + written,
                        buffer_size > (size_t) written ? buffer_size - (size_t) written : 0,
                        "v=0\r\n"
                        "o=sipserver 1 1 IN IP4 %s\r\n"
                        "s=Minimal SIP Server\r\n"
                        "c=IN IP4 %s\r\n"
                        "t=0 0\r\n",
                        streamer->config->media_ip,
                        streamer->config->media_ip);

    for (index = 0; index < plan->media_count; ++index) {
        const streamer_sdp_media_t *media = &plan->media[index];
        uint16_t local_port = 0;

        if (!media->accepted) {
            written += snprintf(buffer + written,
                                buffer_size > (size_t) written ? buffer_size - (size_t) written : 0,
                                "m=%s 0 %s %u\r\n"
                                "a=inactive\r\n",
                                media->kind == STREAMER_MEDIA_AUDIO ? "audio" : "video",
                                media->transport,
                                media->payload_type);
            continue;
        }

        if (media->kind == STREAMER_MEDIA_AUDIO) {
            local_port = streamer->config->audio_port;
            if (streamer->config->audio_codec == AUDIO_CODEC_AAC) {
                written += snprintf(buffer + written,
                                    buffer_size > (size_t) written ? buffer_size - (size_t) written : 0,
                                    "m=audio %u %s %u\r\n"
                                    "a=rtpmap:%u MPEG4-GENERIC/%d/%d\r\n"
                                    "a=fmtp:%u streamtype=5;profile-level-id=1;mode=AAC-hbr;"
                                    "config=%s;SizeLength=13;IndexLength=3;IndexDeltaLength=3\r\n",
                                    local_port,
                                    media->transport,
                                    media->payload_type,
                                    media->payload_type,
                                    DEFAULT_AAC_SAMPLE_RATE,
                                    DEFAULT_AAC_CHANNELS,
                                    media->payload_type,
                                    DEFAULT_AAC_CONFIG_HEX);
            } else {
                written += snprintf(buffer + written,
                                    buffer_size > (size_t) written ? buffer_size - (size_t) written : 0,
                                    "m=audio %u %s %u\r\n"
                                    "a=rtpmap:%u PCMA/8000\r\n",
                                    local_port,
                                    media->transport,
                                    media->payload_type,
                                    media->payload_type);
            }
        } else {
            local_port = streamer->config->video_port;
            written += snprintf(buffer + written,
                                buffer_size > (size_t) written ? buffer_size - (size_t) written : 0,
                                "m=video %u %s %u\r\n"
                                "a=rtpmap:%u H264/90000\r\n"
                                "a=fmtp:%u packetization-mode=1;profile-level-id=%s\r\n",
                                local_port,
                                media->transport,
                                media->payload_type,
                                media->payload_type,
                                media->payload_type,
                                DEFAULT_H264_PROFILE_LEVEL_ID);
        }

        written += snprintf(buffer + written,
                            buffer_size > (size_t) written ? buffer_size - (size_t) written : 0,
                            "a=%s\r\n",
                            streamer_direction_name(media->direction));
    }

    return (size_t) written < buffer_size ? 0 : -1;
}
