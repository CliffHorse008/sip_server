#include "sip_server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdint.h>
#include <unistd.h>

#include "net.h"
#include "streamer.h"

#define SIP_BUFFER_SIZE 8192
#define SDP_MAX_PAYLOADS 16

typedef struct {
    char method[16];
    char via[1024];
    char from[1024];
    char to[1024];
    char call_id[256];
    int cseq;
    char cseq_method[16];
    char source_ip[64];
    uint16_t source_port;
    const char *body;
} sip_request_t;

typedef struct {
    int active;
    int established;
    char call_id[256];
    char from[1024];
    char to[1024];
    char via[1024];
    int cseq;
    char cseq_method[16];
    char local_tag[32];
    char remote_ip[64];
    char remote_ip_alt[64];
    uint16_t remote_audio_port;
    uint16_t remote_video_port;
    uint8_t remote_audio_payload_type;
    uint8_t remote_video_payload_type;
} sip_dialog_t;

typedef struct {
    unsigned int payload_type;
    char encoding[32];
    unsigned int clock_rate;
    unsigned int channels;
    char fmtp[256];
} sdp_payload_desc_t;

typedef struct {
    streamer_media_kind_t kind;
    uint16_t port;
    char transport[32];
    unsigned int payloads[SDP_MAX_PAYLOADS];
    size_t payload_count;
    sdp_payload_desc_t formats[SDP_MAX_PAYLOADS];
    size_t format_count;
    streamer_direction_t direction;
} sdp_media_offer_t;

typedef struct {
    char connection_ip[64];
    sdp_media_offer_t media[STREAMER_SDP_MAX_MEDIA];
    size_t media_count;
} sdp_offer_t;

static int str_case_equal(const char *lhs, const char *rhs)
{
    while (*lhs != '\0' && *rhs != '\0') {
        if (tolower((unsigned char) *lhs) != tolower((unsigned char) *rhs)) {
            return 0;
        }
        ++lhs;
        ++rhs;
    }

    return *lhs == '\0' && *rhs == '\0';
}

static int str_case_prefix(const char *line, const char *prefix, size_t prefix_len)
{
    size_t index;

    for (index = 0; index < prefix_len; ++index) {
        if (line[index] == '\0') {
            return 0;
        }
        if (tolower((unsigned char) line[index]) != tolower((unsigned char) prefix[index])) {
            return 0;
        }
    }

    return 1;
}

static void trim_copy(char *dst, size_t dst_size, const char *begin, size_t length)
{
    size_t start = 0;
    size_t end = length;

    while (start < length && isspace((unsigned char) begin[start])) {
        ++start;
    }
    while (end > start && isspace((unsigned char) begin[end - 1])) {
        --end;
    }

    if (dst_size == 0) {
        return;
    }

    length = end - start;
    if (length >= dst_size) {
        length = dst_size - 1;
    }

    memcpy(dst, begin + start, length);
    dst[length] = '\0';
}

static int get_header_value(const char *message, const char *header_name, char *buffer, size_t buffer_size)
{
    const char *line = strstr(message, "\r\n");
    size_t header_len = strlen(header_name);

    if (line == NULL) {
        return 0;
    }

    line += 2;
    while (*line != '\0') {
        const char *line_end = strstr(line, "\r\n");
        const char *colon;

        if (line_end == NULL || line_end == line) {
            break;
        }

        colon = memchr(line, ':', (size_t) (line_end - line));
        if (colon != NULL && (size_t) (colon - line) == header_len &&
            str_case_prefix(line, header_name, header_len)) {
            trim_copy(buffer, buffer_size, colon + 1, (size_t) (line_end - colon - 1));
            return 1;
        }

        line = line_end + 2;
    }

    return 0;
}

static const char *get_body(const char *message)
{
    const char *separator = strstr(message, "\r\n\r\n");

    return separator == NULL ? NULL : separator + 4;
}

static int parse_request(const char *message, const struct sockaddr_in *peer, sip_request_t *request)
{
    char first_line[256];
    const char *line_end = strstr(message, "\r\n");

    memset(request, 0, sizeof(*request));
    sockaddr_to_ip_string(peer, request->source_ip, sizeof(request->source_ip));
    request->source_port = ntohs(peer->sin_port);

    if (line_end == NULL || (size_t) (line_end - message) >= sizeof(first_line)) {
        return -1;
    }

    memcpy(first_line, message, (size_t) (line_end - message));
    first_line[line_end - message] = '\0';

    if (sscanf(first_line, "%15s", request->method) != 1) {
        return -1;
    }

    if (!get_header_value(message, "Via", request->via, sizeof(request->via)) ||
        !get_header_value(message, "From", request->from, sizeof(request->from)) ||
        !get_header_value(message, "To", request->to, sizeof(request->to)) ||
        !get_header_value(message, "Call-ID", request->call_id, sizeof(request->call_id))) {
        return -1;
    }

    if (get_header_value(message, "CSeq", first_line, sizeof(first_line))) {
        sscanf(first_line, "%d %15s", &request->cseq, request->cseq_method);
    }

    request->body = get_body(message);
    return 0;
}

static void build_to_header(const char *original_to, const char *local_tag, char *buffer, size_t buffer_size)
{
    if (strstr(original_to, ";tag=") != NULL) {
        snprintf(buffer, buffer_size, "%s", original_to);
        return;
    }

    snprintf(buffer, buffer_size, "%s;tag=%s", original_to, local_tag);
}

static int send_response(int sock,
                         const struct sockaddr_in *peer,
                         const sip_request_t *request,
                         int status_code,
                         const char *reason_phrase,
                         const char *local_tag,
                         const app_config_t *config,
                         const char *extra_headers,
                         const char *body)
{
    char packet[SIP_BUFFER_SIZE];
    char to_header[1100];
    size_t body_length = body == NULL ? 0U : strlen(body);
    int written;

    build_to_header(request->to, local_tag, to_header, sizeof(to_header));

    written = snprintf(packet,
                       sizeof(packet),
                       "SIP/2.0 %d %s\r\n"
                       "Via: %s\r\n"
                       "From: %s\r\n"
                       "To: %s\r\n"
                       "Call-ID: %s\r\n"
                       "CSeq: %d %s\r\n"
                       "Contact: <sip:sipserver@%s:%u>\r\n"
                       "Server: sipserver/0.1\r\n"
                       "%s"
                       "%s"
                       "Content-Length: %zu\r\n"
                       "\r\n"
                       "%s",
                       status_code,
                       reason_phrase,
                       request->via,
                       request->from,
                       to_header,
                       request->call_id,
                       request->cseq,
                       request->cseq_method[0] == '\0' ? request->method : request->cseq_method,
                       config->media_ip,
                       config->sip_port,
                       body != NULL ? "Content-Type: application/sdp\r\n" : "",
                       extra_headers != NULL ? extra_headers : "",
                       body_length,
                       body != NULL ? body : "");

    if (written < 0 || (size_t) written >= sizeof(packet)) {
        fprintf(stderr, "SIP response buffer too small\n");
        return -1;
    }

    if (sendto(sock, packet, (size_t) written, 0, (const struct sockaddr *) peer, sizeof(*peer)) < 0) {
        perror("sendto");
        return -1;
    }

    return 0;
}

static void generate_local_tag(char *buffer, size_t buffer_size)
{
    unsigned int value = (unsigned int) rand();
    snprintf(buffer, buffer_size, "%08x", value);
}

static streamer_direction_t parse_sdp_direction(const char *value)
{
    if (str_case_equal(value, "sendonly")) {
        return STREAMER_DIRECTION_SENDONLY;
    }
    if (str_case_equal(value, "recvonly")) {
        return STREAMER_DIRECTION_RECVONLY;
    }
    if (str_case_equal(value, "inactive")) {
        return STREAMER_DIRECTION_INACTIVE;
    }
    return STREAMER_DIRECTION_SENDRECV;
}

static int is_sdp_direction_attribute(const char *value)
{
    return str_case_equal(value, "sendrecv") ||
           str_case_equal(value, "sendonly") ||
           str_case_equal(value, "recvonly") ||
           str_case_equal(value, "inactive");
}

static int sdp_transport_supported(const char *transport)
{
    return str_case_equal(transport, "RTP/AVP") || str_case_equal(transport, "RTP/AVPF");
}

static sdp_payload_desc_t *find_payload_desc(sdp_media_offer_t *media, unsigned int payload_type)
{
    size_t index;

    for (index = 0; index < media->format_count; ++index) {
        if (media->formats[index].payload_type == payload_type) {
            return &media->formats[index];
        }
    }

    if (media->format_count >= SDP_MAX_PAYLOADS) {
        return NULL;
    }

    media->formats[media->format_count].payload_type = payload_type;
    media->formats[media->format_count].channels = 1;
    ++media->format_count;
    return &media->formats[media->format_count - 1];
}

static const sdp_payload_desc_t *find_payload_desc_const(const sdp_media_offer_t *media, unsigned int payload_type)
{
    size_t index;

    for (index = 0; index < media->format_count; ++index) {
        if (media->formats[index].payload_type == payload_type) {
            return &media->formats[index];
        }
    }

    return NULL;
}

static int parse_sdp_media_line(const char *line, size_t line_len, sdp_offer_t *offer, streamer_direction_t session_direction)
{
    char buffer[512];
    char *saveptr = NULL;
    char *token;
    sdp_media_offer_t *media;

    if (offer->media_count >= STREAMER_SDP_MAX_MEDIA) {
        return 0;
    }

    trim_copy(buffer, sizeof(buffer), line + 2, line_len - 2);
    token = strtok_r(buffer, " ", &saveptr);
    if (token == NULL) {
        return 0;
    }

    media = &offer->media[offer->media_count];
    memset(media, 0, sizeof(*media));
    media->direction = session_direction;

    if (str_case_equal(token, "audio")) {
        media->kind = STREAMER_MEDIA_AUDIO;
    } else if (str_case_equal(token, "video")) {
        media->kind = STREAMER_MEDIA_VIDEO;
    } else {
        return 0;
    }

    token = strtok_r(NULL, " ", &saveptr);
    if (token == NULL) {
        return 0;
    }
    media->port = (uint16_t) strtoul(token, NULL, 10);

    token = strtok_r(NULL, " ", &saveptr);
    if (token == NULL) {
        return 0;
    }
    snprintf(media->transport, sizeof(media->transport), "%s", token);

    while ((token = strtok_r(NULL, " ", &saveptr)) != NULL) {
        if (media->payload_count >= SDP_MAX_PAYLOADS) {
            break;
        }
        media->payloads[media->payload_count++] = strtoul(token, NULL, 10);
    }

    ++offer->media_count;
    return 1;
}

static void parse_sdp_rtpmap(sdp_media_offer_t *media, const char *line, size_t line_len)
{
    char buffer[256];
    char encoding[32];
    unsigned int payload_type = 0;
    unsigned int clock_rate = 0;
    unsigned int channels = 1;
    sdp_payload_desc_t *desc;

    trim_copy(buffer, sizeof(buffer), line + 9, line_len - 9);
    if (sscanf(buffer, "%u %31[^/]/%u/%u", &payload_type, encoding, &clock_rate, &channels) < 3 &&
        sscanf(buffer, "%u %31[^/]/%u", &payload_type, encoding, &clock_rate) < 3) {
        return;
    }

    desc = find_payload_desc(media, payload_type);
    if (desc == NULL) {
        return;
    }

    snprintf(desc->encoding, sizeof(desc->encoding), "%s", encoding);
    desc->clock_rate = clock_rate;
    desc->channels = channels;
}

static void parse_sdp_fmtp(sdp_media_offer_t *media, const char *line, size_t line_len)
{
    char buffer[320];
    unsigned int payload_type = 0;
    char params[256];
    sdp_payload_desc_t *desc;

    trim_copy(buffer, sizeof(buffer), line + 7, line_len - 7);
    if (sscanf(buffer, "%u %255[^\r\n]", &payload_type, params) != 2) {
        return;
    }

    desc = find_payload_desc(media, payload_type);
    if (desc == NULL) {
        return;
    }

    snprintf(desc->fmtp, sizeof(desc->fmtp), "%s", params);
}

static void parse_sdp_offer(const char *body, const char *fallback_ip, sdp_offer_t *offer)
{
    const char *cursor = body;
    sdp_media_offer_t *current_media = NULL;
    streamer_direction_t session_direction = STREAMER_DIRECTION_SENDRECV;

    memset(offer, 0, sizeof(*offer));
    snprintf(offer->connection_ip, sizeof(offer->connection_ip), "%s", fallback_ip);

    if (body == NULL) {
        return;
    }

    while (*cursor != '\0') {
        const char *line_end = strstr(cursor, "\r\n");
        size_t line_len = line_end == NULL ? strlen(cursor) : (size_t) (line_end - cursor);

        if (line_len >= 9 && strncmp(cursor, "c=IN IP4 ", 9) == 0) {
            trim_copy(offer->connection_ip, sizeof(offer->connection_ip), cursor + 9, line_len - 9);
        } else if (line_len >= 2 && strncmp(cursor, "m=", 2) == 0) {
            current_media = parse_sdp_media_line(cursor, line_len, offer, session_direction) != 0
                                ? &offer->media[offer->media_count - 1]
                                : NULL;
        } else if (line_len >= 9 && strncmp(cursor, "a=rtpmap:", 9) == 0 && current_media != NULL) {
            parse_sdp_rtpmap(current_media, cursor, line_len);
        } else if (line_len >= 7 && strncmp(cursor, "a=fmtp:", 7) == 0 && current_media != NULL) {
            parse_sdp_fmtp(current_media, cursor, line_len);
        } else if (line_len >= 2 && strncmp(cursor, "a=", 2) == 0) {
            char attribute[32];

            trim_copy(attribute, sizeof(attribute), cursor + 2, line_len - 2);
            if (is_sdp_direction_attribute(attribute)) {
                if (current_media != NULL) {
                    current_media->direction = parse_sdp_direction(attribute);
                } else {
                    session_direction = parse_sdp_direction(attribute);
                }
            }
        }

        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 2;
    }
}

static int media_payload_is_pcma(const sdp_media_offer_t *media, unsigned int payload_type)
{
    const sdp_payload_desc_t *desc = find_payload_desc_const(media, payload_type);

    if (payload_type == 8) {
        return 1;
    }

    return desc != NULL && str_case_equal(desc->encoding, "PCMA") && desc->clock_rate == 8000;
}

static int media_payload_is_h264(const sdp_media_offer_t *media, unsigned int payload_type)
{
    const sdp_payload_desc_t *desc = find_payload_desc_const(media, payload_type);

    if (desc == NULL || !str_case_equal(desc->encoding, "H264")) {
        return 0;
    }

    if (desc->clock_rate != 0 && desc->clock_rate != 90000) {
        return 0;
    }

    return 1;
}

static int choose_audio_payload(const sdp_media_offer_t *media, const app_config_t *config, uint8_t *payload_type)
{
    size_t index;

    if (config->audio_codec != AUDIO_CODEC_G711A) {
        return 0;
    }

    for (index = 0; index < media->payload_count; ++index) {
        if (media_payload_is_pcma(media, media->payloads[index])) {
            *payload_type = (uint8_t) media->payloads[index];
            return 1;
        }
    }

    return 0;
}

static int choose_video_payload(const sdp_media_offer_t *media, uint8_t *payload_type)
{
    size_t index;

    for (index = 0; index < media->payload_count; ++index) {
        if (media_payload_is_h264(media, media->payloads[index])) {
            *payload_type = (uint8_t) media->payloads[index];
            return 1;
        }
    }

    return 0;
}

static int build_sdp_plan(const sdp_offer_t *offer,
                          const app_config_t *config,
                          streamer_sdp_plan_t *plan,
                          char *remote_ip,
                          size_t remote_ip_size,
                          uint16_t *audio_port,
                          uint16_t *video_port,
                          uint8_t *audio_payload_type,
                          uint8_t *video_payload_type)
{
    size_t index;
    int audio_selected = 0;
    int video_selected = 0;
    int accepted_media = 0;

    memset(plan, 0, sizeof(*plan));
    snprintf(remote_ip, remote_ip_size, "%s", offer->connection_ip);
    *audio_port = 0;
    *video_port = 0;
    *audio_payload_type = 0;
    *video_payload_type = 96;

    for (index = 0; index < offer->media_count; ++index) {
        const sdp_media_offer_t *offered = &offer->media[index];
        streamer_sdp_media_t *planned = &plan->media[plan->media_count++];

        memset(planned, 0, sizeof(*planned));
        planned->kind = offered->kind;
        planned->payload_type = offered->payload_count > 0 ? (uint8_t) offered->payloads[0] : 0;
        planned->direction = offered->direction == STREAMER_DIRECTION_RECVONLY
                                 ? STREAMER_DIRECTION_SENDONLY
                                 : STREAMER_DIRECTION_SENDRECV;
        snprintf(planned->transport, sizeof(planned->transport), "%s", offered->transport);

        if (!sdp_transport_supported(offered->transport) || offered->port == 0) {
            continue;
        }

        if (offered->direction == STREAMER_DIRECTION_SENDONLY || offered->direction == STREAMER_DIRECTION_INACTIVE) {
            continue;
        }

        if (offered->kind == STREAMER_MEDIA_AUDIO) {
            uint8_t payload = 0;

            if (audio_selected || !choose_audio_payload(offered, config, &payload)) {
                continue;
            }

            planned->accepted = 1;
            planned->payload_type = payload;
            *audio_port = offered->port;
            *audio_payload_type = payload;
            audio_selected = 1;
            ++accepted_media;
        } else {
            uint8_t payload = 0;

            if (video_selected || !choose_video_payload(offered, &payload)) {
                continue;
            }

            planned->accepted = 1;
            planned->payload_type = payload;
            *video_port = offered->port;
            *video_payload_type = payload;
            video_selected = 1;
            ++accepted_media;
        }
    }

    return accepted_media;
}

static const char *media_kind_name(streamer_media_kind_t kind)
{
    return kind == STREAMER_MEDIA_AUDIO ? "audio" : "video";
}

static void log_sdp_offer(const sdp_offer_t *offer)
{
    size_t media_index;

    fprintf(stdout, "SDP offer connection=%s media_count=%zu\n", offer->connection_ip, offer->media_count);

    for (media_index = 0; media_index < offer->media_count; ++media_index) {
        const sdp_media_offer_t *media = &offer->media[media_index];
        size_t payload_index;

        fprintf(stdout,
                "offer %s port=%u transport=%s direction=%d payloads=",
                media_kind_name(media->kind),
                media->port,
                media->transport,
                media->direction);

        for (payload_index = 0; payload_index < media->payload_count; ++payload_index) {
            const sdp_payload_desc_t *desc = find_payload_desc_const(media, media->payloads[payload_index]);

            fprintf(stdout,
                    "%s%u%s%s",
                    payload_index == 0 ? "" : ",",
                    media->payloads[payload_index],
                    desc != NULL && desc->encoding[0] != '\0' ? ":" : "",
                    desc != NULL ? desc->encoding : "");
        }
        fputc('\n', stdout);
    }
}

static void log_sdp_plan(const streamer_sdp_plan_t *plan,
                         const char *remote_ip,
                         uint16_t audio_port,
                         uint16_t video_port,
                         uint8_t audio_payload_type,
                         uint8_t video_payload_type)
{
    size_t media_index;

    fprintf(stdout,
            "SDP plan remote_ip=%s audio=%u(pt=%u) video=%u(pt=%u) media_count=%zu\n",
            remote_ip,
            audio_port,
            audio_payload_type,
            video_port,
            video_payload_type,
            plan->media_count);

    for (media_index = 0; media_index < plan->media_count; ++media_index) {
        const streamer_sdp_media_t *media = &plan->media[media_index];

        fprintf(stdout,
                "plan %s accepted=%d transport=%s pt=%u direction=%d\n",
                media_kind_name(media->kind),
                media->accepted,
                media->transport,
                media->payload_type,
                media->direction);
    }
}

static void dialog_reset(sip_dialog_t *dialog)
{
    memset(dialog, 0, sizeof(*dialog));
}

static int dialog_matches(const sip_dialog_t *dialog, const sip_request_t *request)
{
    return dialog->active && strcmp(dialog->call_id, request->call_id) == 0;
}

int sip_server_run_with_callback(const app_config_t *config,
                                 volatile sig_atomic_t *stop_flag,
                                 streamer_receive_callback_t media_callback,
                                 void *media_user_data)
{
    int sip_socket;
    struct timeval timeout;
    streamer_t *streamer;
    sip_dialog_t dialog;

    streamer = streamer_create(config);
    if (streamer == NULL) {
        return 1;
    }
    streamer_set_receive_callback(streamer, media_callback, media_user_data);

    sip_socket = udp_socket_bind(config->bind_ip, config->sip_port);
    if (sip_socket < 0) {
        streamer_destroy(streamer);
        return 1;
    }

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (setsockopt(sip_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        perror("setsockopt(SO_RCVTIMEO)");
    }

    dialog_reset(&dialog);

    while (*stop_flag == 0) {
        char buffer[SIP_BUFFER_SIZE];
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t received = recvfrom(sip_socket, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *) &peer, &peer_len);

        if (received < 0) {
            continue;
        }

        buffer[received] = '\0';

        {
            sip_request_t request;

            if (parse_request(buffer, &peer, &request) != 0) {
                fprintf(stderr, "ignoring malformed SIP message\n");
                continue;
            }

            fprintf(stdout, "SIP %s from %s:%u\n", request.method, request.source_ip, request.source_port);

            if (str_case_equal(request.method, "OPTIONS") || str_case_equal(request.method, "REGISTER")) {
                char local_tag[32];
                generate_local_tag(local_tag, sizeof(local_tag));
                send_response(sip_socket,
                              &peer,
                              &request,
                              200,
                              "OK",
                              local_tag,
                              config,
                              "Allow: INVITE, ACK, BYE, OPTIONS, REGISTER\r\n",
                              NULL);
            } else if (str_case_equal(request.method, "INVITE")) {
                char sdp[2048];
                sdp_offer_t offer;
                streamer_sdp_plan_t plan;
                int accepted_media_count;

                streamer_stop(streamer);
                dialog_reset(&dialog);
                generate_local_tag(dialog.local_tag, sizeof(dialog.local_tag));

                send_response(sip_socket, &peer, &request, 100, "Trying", dialog.local_tag, config, NULL, NULL);

                parse_sdp_offer(request.body, request.source_ip, &offer);
                log_sdp_offer(&offer);
                accepted_media_count = build_sdp_plan(&offer,
                                                      config,
                                                      &plan,
                                                      dialog.remote_ip,
                                                      sizeof(dialog.remote_ip),
                                                      &dialog.remote_audio_port,
                                                      &dialog.remote_video_port,
                                                      &dialog.remote_audio_payload_type,
                                                      &dialog.remote_video_payload_type);
                dialog.remote_ip_alt[0] = '\0';
                log_sdp_plan(&plan,
                             dialog.remote_ip,
                             dialog.remote_audio_port,
                             dialog.remote_video_port,
                             dialog.remote_audio_payload_type,
                             dialog.remote_video_payload_type);

                if (accepted_media_count <= 0) {
                    send_response(sip_socket,
                                  &peer,
                                  &request,
                                  488,
                                  "Not Acceptable Here",
                                  dialog.local_tag,
                                  config,
                                  NULL,
                                  NULL);
                    dialog_reset(&dialog);
                    continue;
                }

                dialog.active = 1;
                snprintf(dialog.call_id, sizeof(dialog.call_id), "%s", request.call_id);
                snprintf(dialog.from, sizeof(dialog.from), "%s", request.from);
                snprintf(dialog.to, sizeof(dialog.to), "%s", request.to);
                snprintf(dialog.via, sizeof(dialog.via), "%s", request.via);
                dialog.cseq = request.cseq;
                snprintf(dialog.cseq_method, sizeof(dialog.cseq_method), "%s", request.cseq_method);

                send_response(sip_socket, &peer, &request, 180, "Ringing", dialog.local_tag, config, NULL, NULL);

                if (streamer_build_sdp(streamer, &plan, sdp, sizeof(sdp)) != 0) {
                    send_response(sip_socket, &peer, &request, 500, "Server Error", dialog.local_tag, config, NULL, NULL);
                    dialog_reset(&dialog);
                    continue;
                }

                send_response(sip_socket,
                              &peer,
                              &request,
                              200,
                              "OK",
                              dialog.local_tag,
                              config,
                              "Allow: INVITE, ACK, BYE, OPTIONS, REGISTER\r\n",
                              sdp);
            } else if (str_case_equal(request.method, "ACK")) {
                if (dialog_matches(&dialog, &request) && !dialog.established) {
                    if (request.source_ip[0] != '\0' && strcmp(request.source_ip, dialog.remote_ip) != 0) {
                        snprintf(dialog.remote_ip_alt, sizeof(dialog.remote_ip_alt), "%s", request.source_ip);
                    }
                    if (streamer_start(streamer,
                                       dialog.remote_ip,
                                       dialog.remote_ip_alt,
                                       dialog.remote_audio_port,
                                       dialog.remote_video_port,
                                       dialog.remote_audio_payload_type,
                                       dialog.remote_video_payload_type) == 0) {
                        dialog.established = 1;
                    }
                }
            } else if (str_case_equal(request.method, "BYE")) {
                if (dialog_matches(&dialog, &request)) {
                    streamer_stop(streamer);
                    send_response(sip_socket, &peer, &request, 200, "OK", dialog.local_tag, config, NULL, NULL);
                    dialog_reset(&dialog);
                } else {
                    char local_tag[32];
                    generate_local_tag(local_tag, sizeof(local_tag));
                    send_response(sip_socket, &peer, &request, 481, "Call/Transaction Does Not Exist", local_tag, config, NULL, NULL);
                }
            } else {
                char local_tag[32];
                generate_local_tag(local_tag, sizeof(local_tag));
                send_response(sip_socket, &peer, &request, 405, "Method Not Allowed", local_tag, config, NULL, NULL);
            }
        }
    }

    streamer_stop(streamer);
    streamer_destroy(streamer);
    close(sip_socket);
    return 0;
}

int sip_server_run(const app_config_t *config, volatile sig_atomic_t *stop_flag)
{
    return sip_server_run_with_callback(config, stop_flag, NULL, NULL);
}
