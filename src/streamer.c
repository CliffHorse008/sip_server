#include "streamer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "net.h"

#define RTP_HEADER_SIZE 12
#define RTP_MAX_PAYLOAD 1200
#define RTP_CLOCK_VIDEO 90000U
#define STREAMER_QUEUE_DEPTH 128
#define DEFAULT_AAC_SAMPLE_RATE 48000
#define DEFAULT_AAC_CHANNELS 2
#define DEFAULT_AAC_CONFIG_HEX "1190"
#define DEFAULT_H264_PROFILE_LEVEL_ID "42E01F"

typedef struct {
    streamer_t *streamer;
    streamer_media_kind_t kind;
    int socket_fd;
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
};

typedef struct {
    int socket_fd;
    struct sockaddr_in remote_addr;
    int has_alt_remote;
    struct sockaddr_in alt_remote_addr;
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

void streamer_stop(streamer_t *streamer);

static unsigned int random_u32(void)
{
    return ((unsigned int) rand() << 16) ^ (unsigned int) rand();
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

static void media_queue_wake_and_stop(media_frame_queue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->stopped = 1;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

static void media_queue_reset(media_frame_queue_t *queue)
{
    media_frame_t *node;
    media_frame_t *next;

    pthread_mutex_lock(&queue->mutex);
    queue->stopped = 0;
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
        media_frame_t *dropped = queue->head;

        queue->head = dropped->next;
        if (queue->head == NULL) {
            queue->tail = NULL;
        }
        --queue->depth;
        free(dropped->data);
        free(dropped);
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

    if (sendto(context->socket_fd,
               packet,
               RTP_HEADER_SIZE + payload_size,
               0,
               (const struct sockaddr *) &context->remote_addr,
               sizeof(context->remote_addr)) < 0) {
        return -1;
    }

    if (context->has_alt_remote) {
        if (sendto(context->socket_fd,
                   packet,
                   RTP_HEADER_SIZE + payload_size,
                   0,
                   (const struct sockaddr *) &context->alt_remote_addr,
                   sizeof(context->alt_remote_addr)) < 0) {
            return -1;
        }
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
                              int socket_fd,
                              uint16_t local_port,
                              const char *remote_ip,
                              const char *remote_ip_alt,
                              uint16_t remote_port,
                              uint32_t initial_timestamp,
                              rtp_context_t *context)
{
    (void) config;
    (void) local_port;
    memset(context, 0, sizeof(*context));
    context->socket_fd = socket_fd;

    if (sockaddr_from_ip_port(remote_ip, remote_port, &context->remote_addr) != 0) {
        return -1;
    }

    if (remote_ip_alt != NULL &&
        remote_ip_alt[0] != '\0' &&
        strcmp(remote_ip_alt, remote_ip) != 0 &&
        sockaddr_from_ip_port(remote_ip_alt, remote_port, &context->alt_remote_addr) == 0) {
        context->has_alt_remote = 1;
    }

    context->sequence = (uint16_t) random_u32();
    context->timestamp = initial_timestamp;
    context->ssrc = random_u32();
    return 0;
}

static void destroy_rtp_context(rtp_context_t *context)
{
    context->socket_fd = -1;
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

static void *receive_thread_main(void *opaque)
{
    receive_thread_args_t *args = (receive_thread_args_t *) opaque;
    streamer_t *streamer = args->streamer;
    unsigned char packet[1600];

    while (!streamer->stop_requested) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t received = recvfrom(args->socket_fd, packet, sizeof(packet), 0, (struct sockaddr *) &peer, &peer_len);

        if (received < 0) {
            continue;
        }

        deliver_received_rtp(streamer, args->kind, packet, (size_t) received, &peer);
    }

    return NULL;
}

static void *video_thread_main(void *opaque)
{
    streamer_t *streamer = (streamer_t *) opaque;
    rtp_context_t context;

    if (create_rtp_context(streamer->config,
                           streamer->video_socket_fd,
                           streamer->config->video_port,
                           streamer->session.remote_ip,
                           streamer->session.remote_ip_alt,
                           streamer->session.video_port,
                           random_u32(),
                           &context) != 0) {
        return NULL;
    }

    fprintf(stdout,
            "video RTP start local=%u remote=%s%s%s:%u pt=%u\n",
            streamer->config->video_port,
            streamer->session.remote_ip,
            streamer->session.remote_ip_alt[0] != '\0' ? " alt=" : "",
            streamer->session.remote_ip_alt[0] != '\0' ? streamer->session.remote_ip_alt : "",
            streamer->session.video_port,
            streamer->session.video_payload_type);

    while (!streamer->stop_requested) {
        media_frame_t frame;

        if (media_queue_pop(streamer->video_queue_ptr, &frame, 10)) {
            context.timestamp = frame.timestamp;
            if (rtp_send_h264_access_unit(&context,
                                          streamer->session.video_payload_type,
                                          frame.data,
                                          frame.size) != 0) {
                perror("sendto(video-live)");
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
                           streamer->audio_socket_fd,
                           streamer->config->audio_port,
                           streamer->session.remote_ip,
                           streamer->session.remote_ip_alt,
                           streamer->session.audio_port,
                           random_u32(),
                           &context) != 0) {
        return NULL;
    }

    fprintf(stdout,
            "audio RTP start local=%u remote=%s%s%s:%u pt=%u codec=%s\n",
            streamer->config->audio_port,
            streamer->session.remote_ip,
            streamer->session.remote_ip_alt[0] != '\0' ? " alt=" : "",
            streamer->session.remote_ip_alt[0] != '\0' ? streamer->session.remote_ip_alt : "",
            streamer->session.audio_port,
            streamer->session.audio_payload_type,
            config_audio_codec_name(streamer->session.audio_codec));

    if (streamer->session.audio_codec == AUDIO_CODEC_AAC) {
        while (!streamer->stop_requested) {
            media_frame_t frame;

            if (media_queue_pop(streamer->audio_queue_ptr, &frame, 10)) {
                context.timestamp = frame.timestamp;
                if (rtp_send_aac_frame(&context,
                                       streamer->session.audio_payload_type,
                                       frame.data,
                                       frame.size) != 0) {
                    perror("sendto(audio-aac-live)");
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

            if (media_queue_pop(streamer->audio_queue_ptr, &frame, 10)) {
                context.timestamp = frame.timestamp;
                if (rtp_send_g711a_frame(&context,
                                         streamer->session.audio_payload_type,
                                         frame.data,
                                         frame.size) != 0) {
                    perror("sendto(audio-g711a-live)");
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

    if (streamer->receive_callback != NULL && params->video_enabled && params->video_port != 0) {
        streamer->video_receive_args.streamer = streamer;
        streamer->video_receive_args.kind = STREAMER_MEDIA_VIDEO;
        streamer->video_receive_args.socket_fd = streamer->video_socket_fd;
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
        streamer->audio_receive_args.socket_fd = streamer->audio_socket_fd;
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
            "media session to %s audio=%u video=%u codec=%s\n",
            streamer->session.remote_ip,
            streamer->session.audio_port,
            streamer->session.video_port,
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
