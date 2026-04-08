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
#define G711A_PACKET_SAMPLES 160U

typedef struct {
    const unsigned char *data;
    size_t length;
    unsigned char nal_type;
} h264_nal_t;

typedef struct {
    const unsigned char *data;
    size_t length;
} aac_frame_t;

typedef struct {
    unsigned char *blob;
    size_t blob_size;
    h264_nal_t *nals;
    size_t nal_count;
    char sprop_parameter_sets[512];
    char profile_level_id[7];
    int enabled;
} h264_track_t;

typedef struct {
    unsigned char *blob;
    size_t blob_size;
    aac_frame_t *frames;
    size_t frame_count;
    int sample_rate;
    int channels;
    int object_type;
    char config_hex[8];
    int enabled;
} aac_track_t;

typedef struct {
    unsigned char *blob;
    size_t blob_size;
    int enabled;
} g711a_track_t;

typedef struct {
    h264_track_t h264;
    aac_track_t aac;
    g711a_track_t g711a;
} media_assets_t;

typedef struct {
    streamer_t *streamer;
    streamer_media_kind_t kind;
    int socket_fd;
} receive_thread_args_t;

struct streamer {
    const app_config_t *config;
    media_assets_t assets;
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
    char remote_ip[64];
    char remote_ip_alt[64];
    uint16_t remote_audio_port;
    uint16_t remote_video_port;
    uint8_t remote_audio_payload_type;
    uint8_t remote_video_payload_type;
    streamer_receive_callback_t receive_callback;
    void *receive_user_data;
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

void streamer_stop(streamer_t *streamer);

static const int k_sample_rate_table[16] = {
    96000, 88200, 64000, 48000,
    44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000,
    7350, 0, 0, 0
};

static unsigned int random_u32(void)
{
    return ((unsigned int) rand() << 16) ^ (unsigned int) rand();
}

static void sleep_ns(long long nanoseconds)
{
    struct timespec ts;

    if (nanoseconds <= 0) {
        return;
    }

    ts.tv_sec = (time_t) (nanoseconds / 1000000000LL);
    ts.tv_nsec = (long) (nanoseconds % 1000000000LL);
    nanosleep(&ts, NULL);
}

static int set_socket_receive_timeout(int socket_fd, int timeout_ms)
{
    struct timeval timeout;

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

static int read_entire_file(const char *path, unsigned char **buffer, size_t *buffer_size)
{
    FILE *file;
    long length;
    unsigned char *data;

    *buffer = NULL;
    *buffer_size = 0;

    file = fopen(path, "rb");
    if (file == NULL) {
        perror(path);
        return -1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }

    length = ftell(file);
    if (length < 0) {
        fclose(file);
        return -1;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    data = (unsigned char *) malloc((size_t) length);
    if (data == NULL) {
        fclose(file);
        return -1;
    }

    if (length > 0 && fread(data, 1, (size_t) length, file) != (size_t) length) {
        fclose(file);
        free(data);
        return -1;
    }

    fclose(file);
    *buffer = data;
    *buffer_size = (size_t) length;
    return 0;
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

static int h264_parse_nals(h264_track_t *track)
{
    size_t offset = 0;
    size_t count = 0;
    size_t capacity = 0;
    h264_nal_t *nals = NULL;

    while (1) {
        size_t start;
        size_t prefix_len;
        size_t next_start;
        size_t next_prefix_len;
        size_t nal_offset;
        size_t nal_end;

        if (!find_start_code(track->blob, track->blob_size, offset, &start, &prefix_len)) {
            break;
        }

        nal_offset = start + prefix_len;
        offset = nal_offset;

        if (!find_start_code(track->blob, track->blob_size, nal_offset, &next_start, &next_prefix_len)) {
            next_start = track->blob_size;
            next_prefix_len = 0;
        }

        (void) next_prefix_len;
        nal_end = next_start;
        while (nal_end > nal_offset && track->blob[nal_end - 1] == 0x00) {
            --nal_end;
        }

        if (nal_end > nal_offset) {
            if (count == capacity) {
                size_t new_capacity = capacity == 0 ? 128 : capacity * 2;
                h264_nal_t *grown = (h264_nal_t *) realloc(nals, new_capacity * sizeof(*nals));
                if (grown == NULL) {
                    free(nals);
                    return -1;
                }
                nals = grown;
                capacity = new_capacity;
            }

            nals[count].data = track->blob + nal_offset;
            nals[count].length = nal_end - nal_offset;
            nals[count].nal_type = track->blob[nal_offset] & 0x1F;
            ++count;
        }

        if (next_start == track->blob_size) {
            break;
        }
        offset = next_start;
    }

    if (count == 0) {
        free(nals);
        return -1;
    }

    track->nals = nals;
    track->nal_count = count;
    return 0;
}

static void base64_encode(const unsigned char *input, size_t input_len, char *output, size_t output_len)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < input_len && out_pos + 4 < output_len) {
        unsigned int value = 0;
        size_t remaining = input_len - in_pos;
        int i;

        for (i = 0; i < 3; ++i) {
            value <<= 8;
            if ((size_t) i < remaining) {
                value |= input[in_pos + (size_t) i];
            }
        }

        output[out_pos++] = alphabet[(value >> 18) & 0x3F];
        output[out_pos++] = alphabet[(value >> 12) & 0x3F];
        output[out_pos++] = remaining > 1 ? alphabet[(value >> 6) & 0x3F] : '=';
        output[out_pos++] = remaining > 2 ? alphabet[value & 0x3F] : '=';
        in_pos += remaining > 3 ? 3 : remaining;
    }

    output[out_pos] = '\0';
}

static void h264_build_fmtp(h264_track_t *track)
{
    const unsigned char *sps = NULL;
    const unsigned char *pps = NULL;
    size_t sps_len = 0;
    size_t pps_len = 0;
    char sps_b64[256];
    char pps_b64[256];
    size_t index;

    snprintf(track->profile_level_id, sizeof(track->profile_level_id), "42E01F");
    track->sprop_parameter_sets[0] = '\0';

    for (index = 0; index < track->nal_count; ++index) {
        if (track->nals[index].nal_type == 7 && sps == NULL) {
            sps = track->nals[index].data;
            sps_len = track->nals[index].length;
        } else if (track->nals[index].nal_type == 8 && pps == NULL) {
            pps = track->nals[index].data;
            pps_len = track->nals[index].length;
        }
    }

    if (sps != NULL && sps_len >= 4) {
        snprintf(track->profile_level_id,
                 sizeof(track->profile_level_id),
                 "%02X%02X%02X",
                 sps[1],
                 sps[2],
                 sps[3]);
    }

    if (sps != NULL && pps != NULL) {
        base64_encode(sps, sps_len, sps_b64, sizeof(sps_b64));
        base64_encode(pps, pps_len, pps_b64, sizeof(pps_b64));
        snprintf(track->sprop_parameter_sets,
                 sizeof(track->sprop_parameter_sets),
                 "%s,%s",
                 sps_b64,
                 pps_b64);
    }
}

static int parse_adts_header(const unsigned char *header,
                             size_t available,
                             size_t *frame_length,
                             size_t *payload_offset,
                             int *sample_rate,
                             int *channels,
                             int *object_type)
{
    int profile;
    int sample_rate_index;
    int channel_config;
    int protection_absent;

    if (available < 7) {
        return -1;
    }

    if (header[0] != 0xFF || (header[1] & 0xF0) != 0xF0) {
        return -1;
    }

    protection_absent = header[1] & 0x01;
    profile = ((header[2] >> 6) & 0x03) + 1;
    sample_rate_index = (header[2] >> 2) & 0x0F;
    channel_config = ((header[2] & 0x01) << 2) | ((header[3] >> 6) & 0x03);
    *frame_length = ((size_t) (header[3] & 0x03) << 11) |
                    ((size_t) header[4] << 3) |
                    ((size_t) (header[5] >> 5) & 0x07);
    *payload_offset = protection_absent ? 7U : 9U;

    if (sample_rate_index < 0 || sample_rate_index >= 16 || k_sample_rate_table[sample_rate_index] == 0) {
        return -1;
    }
    if (*frame_length < *payload_offset || *frame_length > available) {
        return -1;
    }

    *sample_rate = k_sample_rate_table[sample_rate_index];
    *channels = channel_config;
    *object_type = profile;
    return 0;
}

static int aac_parse_frames(aac_track_t *track)
{
    size_t offset = 0;
    size_t count = 0;
    size_t capacity = 0;
    aac_frame_t *frames = NULL;

    while (offset + 7 <= track->blob_size) {
        size_t frame_length;
        size_t payload_offset;
        int sample_rate;
        int channels;
        int object_type;

        if (parse_adts_header(track->blob + offset,
                              track->blob_size - offset,
                              &frame_length,
                              &payload_offset,
                              &sample_rate,
                              &channels,
                              &object_type) != 0) {
            free(frames);
            return -1;
        }

        if (count == capacity) {
            size_t new_capacity = capacity == 0 ? 256 : capacity * 2;
            aac_frame_t *grown = (aac_frame_t *) realloc(frames, new_capacity * sizeof(*frames));
            if (grown == NULL) {
                free(frames);
                return -1;
            }
            frames = grown;
            capacity = new_capacity;
        }

        frames[count].data = track->blob + offset + payload_offset;
        frames[count].length = frame_length - payload_offset;
        ++count;

        if (track->sample_rate == 0) {
            unsigned int audio_specific_config;
            int sample_rate_index = (track->blob[offset + 2] >> 2) & 0x0F;

            track->sample_rate = sample_rate;
            track->channels = channels;
            track->object_type = object_type;
            audio_specific_config = ((unsigned int) object_type << 11) |
                                    ((unsigned int) sample_rate_index << 7) |
                                    ((unsigned int) channels << 3);
            snprintf(track->config_hex, sizeof(track->config_hex), "%04X", audio_specific_config);
        }

        offset += frame_length;
    }

    if (count == 0) {
        free(frames);
        return -1;
    }

    track->frames = frames;
    track->frame_count = count;
    return 0;
}

static void free_assets(media_assets_t *assets)
{
    free(assets->h264.blob);
    free(assets->h264.nals);
    free(assets->aac.blob);
    free(assets->aac.frames);
    free(assets->g711a.blob);
    memset(assets, 0, sizeof(*assets));
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

static int load_assets(media_assets_t *assets, const app_config_t *config)
{
    memset(assets, 0, sizeof(*assets));

    if (read_entire_file(config->video_path, &assets->h264.blob, &assets->h264.blob_size) != 0) {
        fprintf(stderr, "failed to load H264 stream: %s\n", config->video_path);
        free_assets(assets);
        return -1;
    }
    if (h264_parse_nals(&assets->h264) != 0) {
        fprintf(stderr, "failed to parse H264 Annex-B stream\n");
        free_assets(assets);
        return -1;
    }
    h264_build_fmtp(&assets->h264);
    assets->h264.enabled = 1;

    if (read_entire_file(config->aac_path, &assets->aac.blob, &assets->aac.blob_size) == 0) {
        if (aac_parse_frames(&assets->aac) == 0) {
            assets->aac.enabled = 1;
        } else {
            fprintf(stderr, "failed to parse AAC ADTS stream: %s\n", config->aac_path);
            free_assets(assets);
            return -1;
        }
    } else if (config->audio_codec == AUDIO_CODEC_AAC) {
        fprintf(stderr, "AAC file is required for audio_codec=aac\n");
        free_assets(assets);
        return -1;
    }

    if (read_entire_file(config->g711a_path, &assets->g711a.blob, &assets->g711a.blob_size) == 0) {
        assets->g711a.enabled = 1;
    } else if (config->audio_codec == AUDIO_CODEC_G711A) {
        fprintf(stderr, "G711A file is required for audio_codec=g711a\n");
        free_assets(assets);
        return -1;
    }

    return 0;
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
    uint32_t frame_step = (uint32_t) (RTP_CLOCK_VIDEO / streamer->config->video_fps);
    long long frame_interval_ns = (long long) (1000000000.0 / streamer->config->video_fps);
    size_t index = 0;

    if (create_rtp_context(streamer->config,
                           streamer->video_socket_fd,
                           streamer->config->video_port,
                           streamer->remote_ip,
                           streamer->remote_ip_alt,
                           streamer->remote_video_port,
                           random_u32(),
                           &context) != 0) {
        return NULL;
    }

    fprintf(stdout,
            "video RTP start local=%u remote=%s%s%s:%u pt=%u\n",
            streamer->config->video_port,
            streamer->remote_ip,
            streamer->remote_ip_alt[0] != '\0' ? " alt=" : "",
            streamer->remote_ip_alt[0] != '\0' ? streamer->remote_ip_alt : "",
            streamer->remote_video_port,
            streamer->remote_video_payload_type);

        while (!streamer->stop_requested) {
            const h264_nal_t *nal = &streamer->assets.h264.nals[index];
            int is_last_nal_of_au =
                (index + 1 >= streamer->assets.h264.nal_count) ||
                (streamer->assets.h264.nals[index + 1].nal_type == 9);

            if (rtp_send_h264_nal(&context,
                                  streamer->remote_video_payload_type,
                                  nal->data,
                                  nal->length,
                                  is_last_nal_of_au && nal->nal_type != 9) != 0) {
                perror("sendto(video)");
                break;
            }

        if (is_last_nal_of_au && nal->nal_type != 9) {
            sleep_ns(frame_interval_ns);
            context.timestamp += frame_step;
        }

        ++index;
        if (index >= streamer->assets.h264.nal_count) {
            index = 0;
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
                           streamer->remote_ip,
                           streamer->remote_ip_alt,
                           streamer->remote_audio_port,
                           random_u32(),
                           &context) != 0) {
        return NULL;
    }

    fprintf(stdout,
            "audio RTP start local=%u remote=%s%s%s:%u pt=%u codec=%s\n",
            streamer->config->audio_port,
            streamer->remote_ip,
            streamer->remote_ip_alt[0] != '\0' ? " alt=" : "",
            streamer->remote_ip_alt[0] != '\0' ? streamer->remote_ip_alt : "",
            streamer->remote_audio_port,
            streamer->remote_audio_payload_type,
            config_audio_codec_name(streamer->config->audio_codec));

    if (streamer->config->audio_codec == AUDIO_CODEC_AAC) {
        long long interval_ns;
        size_t index = 0;

        interval_ns = (long long) (1024.0 * 1000000000.0 / streamer->assets.aac.sample_rate);

        while (!streamer->stop_requested) {
            const aac_frame_t *frame = &streamer->assets.aac.frames[index];

            if (rtp_send_aac_frame(&context,
                                   streamer->remote_audio_payload_type,
                                   frame->data,
                                   frame->length) != 0) {
                perror("sendto(audio-aac)");
                break;
            }

            sleep_ns(interval_ns);
            context.timestamp += 1024;

            ++index;
            if (index >= streamer->assets.aac.frame_count) {
                index = 0;
            }
        }
    } else {
        size_t offset = 0;

        while (!streamer->stop_requested) {
            size_t remaining = streamer->assets.g711a.blob_size - offset;
            size_t chunk = remaining >= G711A_PACKET_SAMPLES ? G711A_PACKET_SAMPLES : remaining;

            if (chunk == 0) {
                offset = 0;
                continue;
            }

            if (rtp_send_g711a_frame(&context,
                                     streamer->remote_audio_payload_type,
                                     streamer->assets.g711a.blob + offset,
                                     chunk) != 0) {
                perror("sendto(audio-g711a)");
                break;
            }

            sleep_ns(20000000LL);
            context.timestamp += (uint32_t) chunk;

            offset += chunk;
            if (offset >= streamer->assets.g711a.blob_size) {
                offset = 0;
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
    if (load_assets(&streamer->assets, config) != 0) {
        free(streamer);
        return NULL;
    }

    streamer->audio_socket_fd = create_media_socket(config->bind_ip, config->audio_port);
    if (streamer->audio_socket_fd < 0) {
        free_assets(&streamer->assets);
        free(streamer);
        return NULL;
    }

    streamer->video_socket_fd = create_media_socket(config->bind_ip, config->video_port);
    if (streamer->video_socket_fd < 0) {
        close(streamer->audio_socket_fd);
        free_assets(&streamer->assets);
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
    free_assets(&streamer->assets);
    free(streamer);
}

void streamer_stop(streamer_t *streamer)
{
    streamer->stop_requested = 1;

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

int streamer_start(streamer_t *streamer,
                   const char *remote_ip,
                   const char *remote_ip_alt,
                   uint16_t audio_port,
                   uint16_t video_port,
                   uint8_t audio_payload_type,
                   uint8_t video_payload_type)
{
    streamer_stop(streamer);
    streamer->stop_requested = 0;

    snprintf(streamer->remote_ip, sizeof(streamer->remote_ip), "%s", remote_ip);
    snprintf(streamer->remote_ip_alt, sizeof(streamer->remote_ip_alt), "%s", remote_ip_alt != NULL ? remote_ip_alt : "");
    streamer->remote_audio_port = audio_port;
    streamer->remote_video_port = video_port;
    streamer->remote_audio_payload_type = audio_payload_type;
    streamer->remote_video_payload_type = video_payload_type;

    if (streamer->receive_callback != NULL && video_port != 0) {
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

    if (streamer->receive_callback != NULL && audio_port != 0) {
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

    if (video_port != 0 && streamer->assets.h264.enabled) {
        if (pthread_create(&streamer->video_thread, NULL, video_thread_main, streamer) == 0) {
            streamer->video_running = 1;
        } else {
            perror("pthread_create(video)");
            return -1;
        }
    }

    if (audio_port != 0) {
        int has_audio = (streamer->config->audio_codec == AUDIO_CODEC_AAC && streamer->assets.aac.enabled) ||
                        (streamer->config->audio_codec == AUDIO_CODEC_G711A && streamer->assets.g711a.enabled);
        if (has_audio) {
            if (pthread_create(&streamer->audio_thread, NULL, audio_thread_main, streamer) == 0) {
                streamer->audio_running = 1;
            } else {
                perror("pthread_create(audio)");
                streamer_stop(streamer);
                return -1;
            }
        }
    }

    fprintf(stdout,
            "media streaming to %s audio=%u video=%u codec=%s\n",
            streamer->remote_ip,
            streamer->remote_audio_port,
            streamer->remote_video_port,
            config_audio_codec_name(streamer->config->audio_codec));

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
                                    streamer->assets.aac.sample_rate,
                                    streamer->assets.aac.channels,
                                    media->payload_type,
                                    streamer->assets.aac.config_hex);
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
                                "a=fmtp:%u packetization-mode=1;profile-level-id=%s;%s%s\r\n",
                                local_port,
                                media->transport,
                                media->payload_type,
                                media->payload_type,
                                media->payload_type,
                                streamer->assets.h264.profile_level_id,
                                streamer->assets.h264.sprop_parameter_sets[0] != '\0' ? "sprop-parameter-sets=" : "",
                                streamer->assets.h264.sprop_parameter_sets);
        }

        written += snprintf(buffer + written,
                            buffer_size > (size_t) written ? buffer_size - (size_t) written : 0,
                            "a=%s\r\n",
                            streamer_direction_name(media->direction));
    }

    return (size_t) written < buffer_size ? 0 : -1;
}
