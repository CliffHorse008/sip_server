#include "sipserver/sip_server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>

#include "internal/net.h"

#define SIP_BUFFER_SIZE 8192
#define SDP_MAX_PAYLOADS 16
#define SIP_TIMER_T1_MS 500LL
#define SIP_TIMER_T2_MS 4000LL
#define SIP_TRANSACTION_LIFETIME_MS (64LL * SIP_TIMER_T1_MS)
#define SIP_MAX_INVITE_TRANSACTIONS 8
#define SIP_MAX_TERMINATED_DIALOGS 8

typedef struct {
    char method[16];
    char via[1024];
    char via_branch[256];
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
    char source_ip[64];
    uint16_t source_port;
    int cseq;
    char cseq_method[16];
    char local_tag[32];
    streamer_session_params_t media;
} sip_dialog_t;

typedef struct {
    int valid;
    char call_id[256];
    char local_tag[32];
    long long expire_at_ms;
} terminated_dialog_t;

typedef struct {
    int active;
    sip_request_t request;
    struct sockaddr_in peer;
    char local_tag[32];
    int status_code;
    char reason_phrase[64];
    char extra_headers[512];
    char body[4096];
    long long next_retransmit_ms;
    long long retransmit_interval_ms;
    long long expire_at_ms;
} invite_transaction_t;

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

typedef struct {
    char connection_ip[64];
    int audio_present;
    int video_present;
    char audio_transport[32];
    char video_transport[32];
    uint16_t audio_port;
    uint16_t video_port;
    uint8_t audio_payload_type;
    uint8_t video_payload_type;
} sip_offer_summary_t;

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

static long long monotonic_time_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long long) tv.tv_sec * 1000LL + (long long) tv.tv_usec / 1000LL;
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

static void extract_header_param_value(const char *header_value,
                                       const char *param_name,
                                       char *buffer,
                                       size_t buffer_size)
{
    const char *cursor = header_value;
    size_t param_name_len = strlen(param_name);

    if (buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (header_value == NULL || param_name == NULL) {
        return;
    }

    while (*cursor != '\0') {
        while (*cursor != '\0' && *cursor != ';') {
            ++cursor;
        }
        if (*cursor != ';') {
            return;
        }

        ++cursor;
        while (*cursor != '\0' && isspace((unsigned char) *cursor)) {
            ++cursor;
        }

        if (str_case_prefix(cursor, param_name, param_name_len) && cursor[param_name_len] == '=') {
            const char *value = cursor + param_name_len + 1;
            const char *end = value;

            while (*end != '\0' && *end != ';' && *end != ',' && !isspace((unsigned char) *end)) {
                ++end;
            }
            trim_copy(buffer, buffer_size, value, (size_t) (end - value));
            return;
        }
    }
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
    extract_header_param_value(request->via, "branch", request->via_branch, sizeof(request->via_branch));

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

static void build_via_header(const sip_request_t *request, char *buffer, size_t buffer_size)
{
    char temp[1100];
    char *rport_marker;

    snprintf(buffer, buffer_size, "%s", request->via);

    rport_marker = strstr(buffer, ";rport");
    if (rport_marker != NULL && strncmp(rport_marker, ";rport=", 7) != 0) {
        const char *suffix = rport_marker + 6;

        *rport_marker = '\0';
        snprintf(temp, sizeof(temp), "%s;rport=%u%s", buffer, request->source_port, suffix);
        snprintf(buffer, buffer_size, "%s", temp);
    }

    if (request->source_ip[0] != '\0' && strstr(buffer, ";received=") == NULL) {
        snprintf(temp, sizeof(temp), "%s;received=%s", buffer, request->source_ip);
        snprintf(buffer, buffer_size, "%s", temp);
    }
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
    char via_header[1100];
    size_t body_length = body == NULL ? 0U : strlen(body);
    int written;

    build_to_header(request->to, local_tag, to_header, sizeof(to_header));
    build_via_header(request, via_header, sizeof(via_header));

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
                       via_header,
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

static int request_has_local_tag(const sip_request_t *request, const char *local_tag)
{
    char tag_pattern[48];

    if (local_tag[0] == '\0') {
        return 0;
    }

    snprintf(tag_pattern, sizeof(tag_pattern), ";tag=%s", local_tag);
    return strstr(request->to, tag_pattern) != NULL;
}

static int dialog_matches(const sip_dialog_t *dialog, const sip_request_t *request)
{
    return dialog->active &&
           strcmp(dialog->call_id, request->call_id) == 0 &&
           request_has_local_tag(request, dialog->local_tag);
}

static int dialog_matches_relaxed(const sip_dialog_t *dialog, const sip_request_t *request)
{
    return dialog->active &&
           strcmp(dialog->call_id, request->call_id) == 0 &&
           dialog->source_port == request->source_port &&
           strcmp(dialog->source_ip, request->source_ip) == 0;
}

static int dialog_matches_for_termination(const sip_dialog_t *dialog, const sip_request_t *request)
{
    return dialog_matches(dialog, request) || dialog_matches_relaxed(dialog, request);
}

static int dialog_conflicts_with_new_invite(const sip_dialog_t *dialog, const sip_request_t *request)
{
    return dialog->active && !dialog_matches(dialog, request);
}

static int terminated_dialog_matches(const terminated_dialog_t *dialog, const sip_request_t *request)
{
    return dialog->valid &&
           strcmp(dialog->call_id, request->call_id) == 0 &&
           request_has_local_tag(request, dialog->local_tag);
}

static void terminated_dialog_table_cleanup(terminated_dialog_t *dialogs, size_t dialog_count)
{
    long long now_ms = monotonic_time_ms();
    size_t index;

    for (index = 0; index < dialog_count; ++index) {
        if (dialogs[index].valid && now_ms >= dialogs[index].expire_at_ms) {
            memset(&dialogs[index], 0, sizeof(dialogs[index]));
        }
    }
}

static terminated_dialog_t *terminated_dialog_find_match(terminated_dialog_t *dialogs,
                                                         size_t dialog_count,
                                                         const sip_request_t *request)
{
    size_t index;

    for (index = 0; index < dialog_count; ++index) {
        if (terminated_dialog_matches(&dialogs[index], request)) {
            return &dialogs[index];
        }
    }

    return NULL;
}

static terminated_dialog_t *terminated_dialog_alloc_slot(terminated_dialog_t *dialogs, size_t dialog_count)
{
    terminated_dialog_t *oldest = &dialogs[0];
    size_t index;

    for (index = 0; index < dialog_count; ++index) {
        if (!dialogs[index].valid) {
            return &dialogs[index];
        }
        if (dialogs[index].expire_at_ms < oldest->expire_at_ms) {
            oldest = &dialogs[index];
        }
    }

    return oldest;
}

static void terminated_dialog_store(terminated_dialog_t *dialogs,
                                    size_t dialog_count,
                                    const sip_dialog_t *dialog)
{
    terminated_dialog_t *slot = terminated_dialog_alloc_slot(dialogs, dialog_count);

    memset(slot, 0, sizeof(*slot));
    slot->valid = 1;
    snprintf(slot->call_id, sizeof(slot->call_id), "%s", dialog->call_id);
    snprintf(slot->local_tag, sizeof(slot->local_tag), "%s", dialog->local_tag);
    slot->expire_at_ms = monotonic_time_ms() + SIP_TRANSACTION_LIFETIME_MS;
}

static void invite_transaction_reset(invite_transaction_t *transaction)
{
    memset(transaction, 0, sizeof(*transaction));
}

static int invite_transaction_matches(const invite_transaction_t *transaction, const sip_request_t *request)
{
    if (transaction->request.via_branch[0] != '\0' && request->via_branch[0] != '\0') {
        return transaction->active &&
               strcmp(transaction->request.call_id, request->call_id) == 0 &&
               transaction->request.cseq == request->cseq &&
               strcmp(transaction->request.via_branch, request->via_branch) == 0;
    }

    return transaction->active &&
           strcmp(transaction->request.call_id, request->call_id) == 0 &&
           transaction->request.cseq == request->cseq &&
           strcmp(transaction->request.via, request->via) == 0;
}

static int invite_transaction_ack_matches(const invite_transaction_t *transaction, const sip_request_t *request)
{
    return transaction->active &&
           strcmp(transaction->request.call_id, request->call_id) == 0 &&
           transaction->request.cseq == request->cseq &&
           request_has_local_tag(request, transaction->local_tag);
}

static void invite_transaction_table_cleanup(invite_transaction_t *transactions, size_t transaction_count)
{
    long long now_ms = monotonic_time_ms();
    size_t index;

    for (index = 0; index < transaction_count; ++index) {
        if (transactions[index].active && now_ms >= transactions[index].expire_at_ms) {
            invite_transaction_reset(&transactions[index]);
        }
    }
}

static invite_transaction_t *invite_transaction_find_match(invite_transaction_t *transactions,
                                                           size_t transaction_count,
                                                           const sip_request_t *request)
{
    size_t index;

    for (index = 0; index < transaction_count; ++index) {
        if (invite_transaction_matches(&transactions[index], request)) {
            return &transactions[index];
        }
    }

    return NULL;
}

static invite_transaction_t *invite_transaction_find_ack(invite_transaction_t *transactions,
                                                         size_t transaction_count,
                                                         const sip_request_t *request)
{
    size_t index;

    for (index = 0; index < transaction_count; ++index) {
        if (invite_transaction_ack_matches(&transactions[index], request)) {
            return &transactions[index];
        }
    }

    return NULL;
}

static invite_transaction_t *invite_transaction_alloc_slot(invite_transaction_t *transactions, size_t transaction_count)
{
    invite_transaction_t *oldest = &transactions[0];
    size_t index;

    for (index = 0; index < transaction_count; ++index) {
        if (!transactions[index].active) {
            return &transactions[index];
        }
        if (transactions[index].expire_at_ms < oldest->expire_at_ms) {
            oldest = &transactions[index];
        }
    }

    return oldest;
}

static void dialog_init_from_request(sip_dialog_t *dialog, const sip_request_t *request)
{
    dialog->active = 1;
    snprintf(dialog->call_id, sizeof(dialog->call_id), "%s", request->call_id);
    snprintf(dialog->from, sizeof(dialog->from), "%s", request->from);
    snprintf(dialog->to, sizeof(dialog->to), "%s", request->to);
    snprintf(dialog->via, sizeof(dialog->via), "%s", request->via);
    snprintf(dialog->source_ip, sizeof(dialog->source_ip), "%s", request->source_ip);
    dialog->source_port = request->source_port;
    dialog->cseq = request->cseq;
    snprintf(dialog->cseq_method, sizeof(dialog->cseq_method), "%s", request->cseq_method);
}

static void emit_signal_event(const sip_server_handlers_t *handlers,
                              streamer_t *streamer,
                              sip_signal_type_t type,
                              const sip_request_t *request,
                              const sip_dialog_t *dialog,
                              int status_code,
                              const char *reason_phrase,
                              const char *body)
{
    sip_signal_event_t event;

    if (handlers == NULL || handlers->on_signal == NULL) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.streamer = streamer;
    event.status_code = status_code;
    event.reason_phrase = reason_phrase;
    event.body = body;

    if (request != NULL) {
        event.method = request->method;
        event.call_id = request->call_id;
        event.from = request->from;
        event.to = request->to;
        event.source_ip = request->source_ip;
        event.source_port = request->source_port;
    } else if (dialog != NULL) {
        event.call_id = dialog->call_id;
        event.from = dialog->from;
        event.to = dialog->to;
    }

    handlers->on_signal(&event, handlers->user_data);
}

static int send_response_maybe_emit(int sock,
                                    const struct sockaddr_in *peer,
                                    const sip_request_t *request,
                                    int status_code,
                                    const char *reason_phrase,
                                    const char *local_tag,
                                    const app_config_t *config,
                                    const char *extra_headers,
                                    const char *body,
                                    const sip_server_handlers_t *handlers,
                                    streamer_t *streamer,
                                    int emit_response_signal)
{
    int rc = send_response(sock, peer, request, status_code, reason_phrase, local_tag, config, extra_headers, body);

    if (rc == 0 && emit_response_signal) {
        emit_signal_event(handlers,
                          streamer,
                          SIP_SIGNAL_RESPONSE_SENT,
                          request,
                          NULL,
                          status_code,
                          reason_phrase,
                          body);
    }

    return rc;
}

static int send_response_and_emit(int sock,
                                  const struct sockaddr_in *peer,
                                  const sip_request_t *request,
                                  int status_code,
                                  const char *reason_phrase,
                                  const char *local_tag,
                                  const app_config_t *config,
                                  const char *extra_headers,
                                  const char *body,
                                  const sip_server_handlers_t *handlers,
                                  streamer_t *streamer)
{
    return send_response_maybe_emit(sock,
                                    peer,
                                    request,
                                    status_code,
                                    reason_phrase,
                                    local_tag,
                                    config,
                                    extra_headers,
                                    body,
                                    handlers,
                                    streamer,
                                    1);
}

static void invite_transaction_store(invite_transaction_t *transaction,
                                     const sip_request_t *request,
                                     const struct sockaddr_in *peer,
                                     int status_code,
                                     const char *reason_phrase,
                                     const char *local_tag,
                                     const char *extra_headers,
                                     const char *body)
{
    invite_transaction_reset(transaction);
    transaction->active = 1;
    transaction->request = *request;
    transaction->request.body = NULL;
    transaction->peer = *peer;
    transaction->status_code = status_code;
    snprintf(transaction->local_tag, sizeof(transaction->local_tag), "%s", local_tag);
    snprintf(transaction->reason_phrase, sizeof(transaction->reason_phrase), "%s", reason_phrase);
    if (extra_headers != NULL) {
        snprintf(transaction->extra_headers, sizeof(transaction->extra_headers), "%s", extra_headers);
    }
    if (body != NULL) {
        snprintf(transaction->body, sizeof(transaction->body), "%s", body);
    }
    transaction->retransmit_interval_ms = SIP_TIMER_T1_MS;
    transaction->next_retransmit_ms = monotonic_time_ms() + transaction->retransmit_interval_ms;
    transaction->expire_at_ms = monotonic_time_ms() + SIP_TRANSACTION_LIFETIME_MS;
}

static void invite_transaction_store_in_table(invite_transaction_t *transactions,
                                              size_t transaction_count,
                                              const sip_request_t *request,
                                              const struct sockaddr_in *peer,
                                              int status_code,
                                              const char *reason_phrase,
                                              const char *local_tag,
                                              const char *extra_headers,
                                              const char *body)
{
    invite_transaction_t *slot = invite_transaction_alloc_slot(transactions, transaction_count);

    invite_transaction_store(slot,
                             request,
                             peer,
                             status_code,
                             reason_phrase,
                             local_tag,
                             extra_headers,
                             body);
}

static int invite_transaction_send_cached(int sock,
                                          const app_config_t *config,
                                          const sip_server_handlers_t *handlers,
                                          streamer_t *streamer,
                                          invite_transaction_t *transaction)
{
    const char *extra_headers = transaction->extra_headers[0] != '\0' ? transaction->extra_headers : NULL;
    const char *body = transaction->body[0] != '\0' ? transaction->body : NULL;

    return send_response_and_emit(sock,
                                  &transaction->peer,
                                  &transaction->request,
                                  transaction->status_code,
                                  transaction->reason_phrase,
                                  transaction->local_tag,
                                  config,
                                  extra_headers,
                                  body,
                                  handlers,
                                  streamer);
}

static void invite_transaction_mark_acknowledged(invite_transaction_t *transaction)
{
    invite_transaction_reset(transaction);
}

static void invite_transaction_maybe_retransmit(int sock,
                                                const app_config_t *config,
                                                const sip_server_handlers_t *handlers,
                                                streamer_t *streamer,
                                                invite_transaction_t *transaction)
{
    long long now_ms;

    if (!transaction->active) {
        return;
    }

    now_ms = monotonic_time_ms();
    if (now_ms >= transaction->expire_at_ms) {
        invite_transaction_reset(transaction);
        return;
    }

    if (now_ms < transaction->next_retransmit_ms) {
        return;
    }

    if (invite_transaction_send_cached(sock, config, handlers, streamer, transaction) == 0) {
        long long next_interval = transaction->retransmit_interval_ms * 2LL;

        if (next_interval > SIP_TIMER_T2_MS) {
            next_interval = SIP_TIMER_T2_MS;
        }
        transaction->retransmit_interval_ms = next_interval;
        transaction->next_retransmit_ms = now_ms + transaction->retransmit_interval_ms;
    } else {
        transaction->next_retransmit_ms = now_ms + transaction->retransmit_interval_ms;
    }
}

static void invite_transaction_maybe_retransmit_all(int sock,
                                                    const app_config_t *config,
                                                    const sip_server_handlers_t *handlers,
                                                    streamer_t *streamer,
                                                    invite_transaction_t *transactions,
                                                    size_t transaction_count)
{
    size_t index;

    for (index = 0; index < transaction_count; ++index) {
        invite_transaction_maybe_retransmit(sock,
                                            config,
                                            handlers,
                                            streamer,
                                            &transactions[index]);
    }
}

static int default_audio_payload_type(audio_codec_t codec)
{
    return codec == AUDIO_CODEC_G711A ? 8 : 97;
}

static void prepare_invite_response_defaults(const app_config_t *config, sip_invite_response_t *response)
{
    memset(response, 0, sizeof(*response));
    response->send_ringing = 1;
    response->media.audio_codec = config->audio_codec;
    response->media.audio_payload_type = (uint8_t) default_audio_payload_type(config->audio_codec);
    response->media.video_payload_type = 96;
}

static void normalize_invite_response(const sip_request_t *request,
                                      const app_config_t *config,
                                      sip_invite_response_t *response)
{
    if (response->accept) {
        if (response->status_code == 0) {
            response->status_code = 200;
        }
        if (response->reason_phrase[0] == '\0') {
            snprintf(response->reason_phrase, sizeof(response->reason_phrase), "%s", "OK");
        }
        if (response->media.remote_ip[0] == '\0') {
            snprintf(response->media.remote_ip, sizeof(response->media.remote_ip), "%s", request->source_ip);
        }
        if (response->media.audio_payload_type == 0 && response->media.audio_port != 0) {
            response->media.audio_payload_type = (uint8_t) default_audio_payload_type(config->audio_codec);
        }
        if (response->media.video_payload_type == 0 && response->media.video_port != 0) {
            response->media.video_payload_type = 96;
        }
        if (response->media.video_port != 0 && !response->media.video_enabled) {
            response->media.video_enabled = 1;
        }
    } else {
        if (response->status_code == 0) {
            response->status_code = 486;
        }
        if (response->reason_phrase[0] == '\0') {
            snprintf(response->reason_phrase, sizeof(response->reason_phrase), "%s", "Busy Here");
        }
    }
}

static int build_default_invite_response(const sdp_offer_t *offer,
                                         const app_config_t *config,
                                         streamer_t *streamer,
                                         sip_invite_response_t *response)
{
    char remote_ip[64];
    streamer_sdp_plan_t plan;
    int accepted_media_count;

    prepare_invite_response_defaults(config, response);
    response->status_code = 488;
    snprintf(response->reason_phrase, sizeof(response->reason_phrase), "%s", "Not Acceptable Here");

    accepted_media_count = build_sdp_plan(offer,
                                          config,
                                          &plan,
                                          remote_ip,
                                          sizeof(remote_ip),
                                          &response->media.audio_port,
                                          &response->media.video_port,
                                          &response->media.audio_payload_type,
                                          &response->media.video_payload_type);
    log_sdp_plan(&plan,
                 remote_ip,
                response->media.audio_port,
                 response->media.video_port,
                 response->media.audio_payload_type,
                 response->media.video_payload_type);

    if (accepted_media_count <= 0) {
        return 0;
    }

    response->accept = 1;
    response->status_code = 200;
    snprintf(response->reason_phrase, sizeof(response->reason_phrase), "%s", "OK");
    snprintf(response->media.remote_ip, sizeof(response->media.remote_ip), "%s", remote_ip);
    response->media.remote_ip_alt[0] = '\0';
    response->media.audio_codec = config->audio_codec;
    response->media.video_enabled = response->media.video_port != 0;

    if (streamer_build_sdp(streamer, &plan, response->answer_sdp, sizeof(response->answer_sdp)) != 0) {
        return -1;
    }

    return 0;
}

static void summarize_offer_for_upper_layer(const sdp_offer_t *offer,
                                            const app_config_t *config,
                                            sip_offer_summary_t *summary)
{
    size_t index;

    memset(summary, 0, sizeof(*summary));
    snprintf(summary->connection_ip, sizeof(summary->connection_ip), "%s", offer->connection_ip);

    for (index = 0; index < offer->media_count; ++index) {
        const sdp_media_offer_t *media = &offer->media[index];

        if (media->kind == STREAMER_MEDIA_AUDIO) {
            summary->audio_present = 1;
            if (summary->audio_transport[0] == '\0') {
                snprintf(summary->audio_transport, sizeof(summary->audio_transport), "%s", media->transport);
            }
        } else {
            summary->video_present = 1;
            if (summary->video_transport[0] == '\0') {
                snprintf(summary->video_transport, sizeof(summary->video_transport), "%s", media->transport);
            }
        }

        if (!sdp_transport_supported(media->transport) || media->port == 0) {
            continue;
        }

        if (media->kind == STREAMER_MEDIA_AUDIO) {
            uint8_t payload_type = 0;

            if (summary->audio_port == 0 && choose_audio_payload(media, config, &payload_type)) {
                summary->audio_port = media->port;
                summary->audio_payload_type = payload_type;
            }
        } else {
            uint8_t payload_type = 0;

            if (summary->video_port == 0 && choose_video_payload(media, &payload_type)) {
                summary->video_port = media->port;
                summary->video_payload_type = payload_type;
            }
        }
    }
}

int sip_server_run_with_handlers(const app_config_t *config,
                                 volatile sig_atomic_t *stop_flag,
                                 const sip_server_handlers_t *handlers)
{
    int sip_socket;
    struct timeval timeout;
    streamer_t *streamer;
    sip_dialog_t dialog;
    terminated_dialog_t terminated_dialogs[SIP_MAX_TERMINATED_DIALOGS];
    invite_transaction_t invite_transactions[SIP_MAX_INVITE_TRANSACTIONS];

    streamer = streamer_create(config);
    if (streamer == NULL) {
        return 1;
    }
    streamer_set_receive_callback(streamer,
                                  handlers != NULL ? handlers->on_media : NULL,
                                  handlers != NULL ? handlers->user_data : NULL);

    sip_socket = udp_socket_bind(config->bind_ip, config->sip_port);
    if (sip_socket < 0) {
        streamer_destroy(streamer);
        return 1;
    }

    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    if (setsockopt(sip_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        perror("setsockopt(SO_RCVTIMEO)");
    }

    dialog_reset(&dialog);
    memset(terminated_dialogs, 0, sizeof(terminated_dialogs));
    memset(invite_transactions, 0, sizeof(invite_transactions));

    while (*stop_flag == 0) {
        char buffer[SIP_BUFFER_SIZE];
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);

        terminated_dialog_table_cleanup(terminated_dialogs, SIP_MAX_TERMINATED_DIALOGS);
        invite_transaction_table_cleanup(invite_transactions, SIP_MAX_INVITE_TRANSACTIONS);
        invite_transaction_maybe_retransmit_all(sip_socket,
                                                config,
                                                handlers,
                                                streamer,
                                                invite_transactions,
                                                SIP_MAX_INVITE_TRANSACTIONS);
        ssize_t received = recvfrom(sip_socket, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *) &peer, &peer_len);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            continue;
        }

        buffer[received] = '\0';

        {
            sip_request_t request;
            terminated_dialog_t *matched_terminated_bye = NULL;
            int duplicate_bye_after_termination = 0;

            if (parse_request(buffer, &peer, &request) != 0) {
                fprintf(stderr, "ignoring malformed SIP message\n");
                continue;
            }

            if (str_case_equal(request.method, "BYE") && !dialog_matches_for_termination(&dialog, &request)) {
                matched_terminated_bye =
                    terminated_dialog_find_match(terminated_dialogs, SIP_MAX_TERMINATED_DIALOGS, &request);
                duplicate_bye_after_termination = matched_terminated_bye != NULL;
            }

            if (!duplicate_bye_after_termination) {
                fprintf(stdout, "SIP %s from %s:%u\n", request.method, request.source_ip, request.source_port);
            }

            if (str_case_equal(request.method, "INVITE")) {
                invite_transaction_t *matched_invite =
                    invite_transaction_find_match(invite_transactions, SIP_MAX_INVITE_TRANSACTIONS, &request);

                if (matched_invite != NULL) {
                    invite_transaction_send_cached(sip_socket, config, handlers, streamer, matched_invite);
                    continue;
                }
            }

            if (str_case_equal(request.method, "ACK")) {
                invite_transaction_t *acked_invite =
                    invite_transaction_find_ack(invite_transactions, SIP_MAX_INVITE_TRANSACTIONS, &request);

                if (acked_invite != NULL) {
                    invite_transaction_mark_acknowledged(acked_invite);
                }
            }

            if (str_case_equal(request.method, "INVITE") &&
                request_has_local_tag(&request, dialog.local_tag) &&
                !dialog_matches(&dialog, &request)) {
                continue;
            }

            if (str_case_equal(request.method, "INVITE")) {
                emit_signal_event(handlers, streamer, SIP_SIGNAL_INVITE_RECEIVED, &request, NULL, 0, NULL, request.body);
            } else if (str_case_equal(request.method, "ACK")) {
                emit_signal_event(handlers, streamer, SIP_SIGNAL_ACK_RECEIVED, &request, NULL, 0, NULL, request.body);
            } else if (str_case_equal(request.method, "OPTIONS")) {
                emit_signal_event(handlers, streamer, SIP_SIGNAL_OPTIONS_RECEIVED, &request, NULL, 0, NULL, request.body);
            } else if (str_case_equal(request.method, "REGISTER")) {
                emit_signal_event(handlers, streamer, SIP_SIGNAL_REGISTER_RECEIVED, &request, NULL, 0, NULL, request.body);
            }

            if (str_case_equal(request.method, "INVITE") && dialog_conflicts_with_new_invite(&dialog, &request)) {
                char busy_local_tag[32];

                generate_local_tag(busy_local_tag, sizeof(busy_local_tag));
                fprintf(stdout,
                        "rejecting INVITE while dialog is active current_call_id=%s new_call_id=%s\n",
                        dialog.call_id,
                        request.call_id);
                if (send_response_and_emit(sip_socket,
                                           &peer,
                                           &request,
                                           486,
                                           "Busy Here",
                                           busy_local_tag,
                                           config,
                                           NULL,
                                           NULL,
                                           handlers,
                                           streamer) == 0) {
                    invite_transaction_store_in_table(invite_transactions,
                                                      SIP_MAX_INVITE_TRANSACTIONS,
                                                      &request,
                                                      &peer,
                                                      486,
                                                      "Busy Here",
                                                      busy_local_tag,
                                                      NULL,
                                                      NULL);
                }
                continue;
            }

            if (str_case_equal(request.method, "OPTIONS") || str_case_equal(request.method, "REGISTER")) {
                char local_tag[32];
                generate_local_tag(local_tag, sizeof(local_tag));
                send_response_and_emit(sip_socket,
                                       &peer,
                                       &request,
                                       200,
                                       "OK",
                                       local_tag,
                                       config,
                                       "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, REGISTER\r\n",
                                       NULL,
                                       handlers,
                                       streamer);
            } else if (str_case_equal(request.method, "CANCEL")) {
                invite_transaction_t *matched_invite =
                    invite_transaction_find_match(invite_transactions, SIP_MAX_INVITE_TRANSACTIONS, &request);

                if (matched_invite != NULL) {
                    send_response_and_emit(sip_socket,
                                           &peer,
                                           &request,
                                           200,
                                           "OK",
                                           matched_invite->local_tag,
                                           config,
                                           NULL,
                                           NULL,
                                           handlers,
                                           streamer);
                } else {
                    char local_tag[32];

                    generate_local_tag(local_tag, sizeof(local_tag));
                    send_response_and_emit(sip_socket,
                                           &peer,
                                           &request,
                                           481,
                                           "Call/Transaction Does Not Exist",
                                           local_tag,
                                           config,
                                           NULL,
                                           NULL,
                                           handlers,
                                           streamer);
                }
            } else if (str_case_equal(request.method, "INVITE")) {
                sip_invite_event_t invite_event;
                sip_invite_response_t invite_response;
                sdp_offer_t offer;
                sip_offer_summary_t offer_summary;
                int invite_rc;

                streamer_stop(streamer);
                if (dialog.active) {
                    terminated_dialog_store(terminated_dialogs, SIP_MAX_TERMINATED_DIALOGS, &dialog);
                    emit_signal_event(handlers, streamer, SIP_SIGNAL_DIALOG_TERMINATED, NULL, &dialog, 0, NULL, NULL);
                }
                dialog_reset(&dialog);
                memset(invite_transactions, 0, sizeof(invite_transactions));
                generate_local_tag(dialog.local_tag, sizeof(dialog.local_tag));

                if (send_response_and_emit(sip_socket,
                                           &peer,
                                           &request,
                                           100,
                                           "Trying",
                                           dialog.local_tag,
                                           config,
                                           NULL,
                                           NULL,
                                           handlers,
                                           streamer) != 0) {
                    dialog_reset(&dialog);
                    continue;
                }

                parse_sdp_offer(request.body, request.source_ip, &offer);
                log_sdp_offer(&offer);
                summarize_offer_for_upper_layer(&offer, config, &offer_summary);

                prepare_invite_response_defaults(config, &invite_response);
                if (handlers != NULL && handlers->on_invite != NULL) {
                    memset(&invite_event, 0, sizeof(invite_event));
                    invite_event.streamer = streamer;
                    invite_event.raw_message = buffer;
                    invite_event.offer_sdp = request.body;
                    invite_event.call_id = request.call_id;
                    invite_event.from = request.from;
                    invite_event.to = request.to;
                    invite_event.via = request.via;
                    invite_event.source_ip = request.source_ip;
                    invite_event.source_port = request.source_port;
                    invite_event.offer_connection_ip = offer_summary.connection_ip;
                    invite_event.offer_audio_present = offer_summary.audio_present;
                    invite_event.offer_video_present = offer_summary.video_present;
                    invite_event.offer_audio_transport = offer_summary.audio_transport;
                    invite_event.offer_video_transport = offer_summary.video_transport;
                    invite_event.offer_audio_port = offer_summary.audio_port;
                    invite_event.offer_video_port = offer_summary.video_port;
                    invite_event.offer_audio_payload_type = offer_summary.audio_payload_type;
                    invite_event.offer_video_payload_type = offer_summary.video_payload_type;
                    invite_rc = handlers->on_invite(&invite_event, &invite_response, handlers->user_data);
                } else {
                    invite_rc = build_default_invite_response(&offer, config, streamer, &invite_response);
                }

                if (invite_rc != 0) {
                    send_response_and_emit(sip_socket,
                                           &peer,
                                           &request,
                                           500,
                                           "Server Error",
                                           dialog.local_tag,
                                           config,
                                           NULL,
                                           NULL,
                                           handlers,
                                           streamer);
                    dialog_reset(&dialog);
                    continue;
                }

                normalize_invite_response(&request, config, &invite_response);

                if (!invite_response.accept) {
                    if (send_response_and_emit(sip_socket,
                                               &peer,
                                               &request,
                                               invite_response.status_code,
                                               invite_response.reason_phrase,
                                               dialog.local_tag,
                                               config,
                                               NULL,
                                               NULL,
                                               handlers,
                                               streamer) == 0) {
                        invite_transaction_store_in_table(invite_transactions,
                                                          SIP_MAX_INVITE_TRANSACTIONS,
                                                          &request,
                                                          &peer,
                                                          invite_response.status_code,
                                                          invite_response.reason_phrase,
                                                          dialog.local_tag,
                                                          NULL,
                                                          NULL);
                    }
                    dialog_reset(&dialog);
                    continue;
                }

                if (invite_response.answer_sdp[0] == '\0') {
                    if (send_response_and_emit(sip_socket,
                                               &peer,
                                               &request,
                                               500,
                                               "Server Error",
                                               dialog.local_tag,
                                               config,
                                               NULL,
                                               NULL,
                                               handlers,
                                               streamer) == 0) {
                        invite_transaction_store_in_table(invite_transactions,
                                                          SIP_MAX_INVITE_TRANSACTIONS,
                                                          &request,
                                                          &peer,
                                                          500,
                                                          "Server Error",
                                                          dialog.local_tag,
                                                          NULL,
                                                          NULL);
                    }
                    dialog_reset(&dialog);
                    continue;
                }

                dialog_init_from_request(&dialog, &request);
                dialog.media = invite_response.media;

                if (invite_response.send_ringing) {
                    if (send_response_and_emit(sip_socket,
                                               &peer,
                                               &request,
                                               180,
                                               "Ringing",
                                               dialog.local_tag,
                                               config,
                                               NULL,
                                               NULL,
                                               handlers,
                                               streamer) != 0) {
                        dialog_reset(&dialog);
                        continue;
                    }
                }

                if (send_response_and_emit(sip_socket,
                                           &peer,
                                           &request,
                                           invite_response.status_code,
                                           invite_response.reason_phrase,
                                           dialog.local_tag,
                                           config,
                                           "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, REGISTER\r\n",
                                           invite_response.answer_sdp,
                                           handlers,
                                           streamer) != 0) {
                    dialog_reset(&dialog);
                    continue;
                }
                invite_transaction_store_in_table(invite_transactions,
                                                  SIP_MAX_INVITE_TRANSACTIONS,
                                                  &request,
                                                  &peer,
                                                  invite_response.status_code,
                                                  invite_response.reason_phrase,
                                                  dialog.local_tag,
                                                  "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, REGISTER\r\n",
                                                  invite_response.answer_sdp);
            } else if (str_case_equal(request.method, "ACK")) {
                if (dialog_matches(&dialog, &request) && !dialog.established) {
                    if (request.source_ip[0] != '\0' && strcmp(request.source_ip, dialog.media.remote_ip) != 0) {
                        snprintf(dialog.media.remote_ip_alt, sizeof(dialog.media.remote_ip_alt), "%s", request.source_ip);
                    }
                    if (streamer_start(streamer, &dialog.media) == 0) {
                        dialog.established = 1;
                        emit_signal_event(handlers,
                                          streamer,
                                          SIP_SIGNAL_DIALOG_ESTABLISHED,
                                          &request,
                                          &dialog,
                                          0,
                                          NULL,
                                          NULL);
                    }
                }
            } else if (str_case_equal(request.method, "BYE")) {
                if (dialog_matches_for_termination(&dialog, &request)) {
                    if (!dialog_matches(&dialog, &request)) {
                        fprintf(stdout,
                                "BYE matched active dialog by relaxed identity call_id=%s source=%s:%u\n",
                                request.call_id,
                                request.source_ip,
                                request.source_port);
                    }
                    emit_signal_event(handlers, streamer, SIP_SIGNAL_BYE_RECEIVED, &request, NULL, 0, NULL, request.body);
                    streamer_stop(streamer);
                    send_response_and_emit(sip_socket,
                                           &peer,
                                           &request,
                                           200,
                                           "OK",
                                           dialog.local_tag,
                                           config,
                                           NULL,
                                           NULL,
                                           handlers,
                                           streamer);
                    terminated_dialog_store(terminated_dialogs, SIP_MAX_TERMINATED_DIALOGS, &dialog);
                    emit_signal_event(handlers, streamer, SIP_SIGNAL_DIALOG_TERMINATED, &request, &dialog, 0, NULL, NULL);
                    dialog_reset(&dialog);
                } else {
                    if (matched_terminated_bye == NULL) {
                        matched_terminated_bye =
                            terminated_dialog_find_match(terminated_dialogs, SIP_MAX_TERMINATED_DIALOGS, &request);
                    }

                    if (matched_terminated_bye != NULL) {
                        send_response_maybe_emit(sip_socket,
                                                 &peer,
                                                 &request,
                                                 200,
                                                 "OK",
                                                 matched_terminated_bye->local_tag,
                                                 config,
                                                 NULL,
                                                 NULL,
                                                 handlers,
                                                 streamer,
                                                 0);
                    } else {
                        char local_tag[32];

                        generate_local_tag(local_tag, sizeof(local_tag));
                        send_response_and_emit(sip_socket,
                                               &peer,
                                               &request,
                                               481,
                                               "Call/Transaction Does Not Exist",
                                               local_tag,
                                               config,
                                               NULL,
                                               NULL,
                                               handlers,
                                               streamer);
                    }
                }
            } else {
                char local_tag[32];
                generate_local_tag(local_tag, sizeof(local_tag));
                send_response_and_emit(sip_socket,
                                       &peer,
                                       &request,
                                       405,
                                       "Method Not Allowed",
                                       local_tag,
                                       config,
                                       NULL,
                                       NULL,
                                       handlers,
                                       streamer);
            }
        }
    }

    streamer_stop(streamer);
    streamer_destroy(streamer);
    close(sip_socket);
    return 0;
}

int sip_server_run_with_callback(const app_config_t *config,
                                 volatile sig_atomic_t *stop_flag,
                                 streamer_receive_callback_t media_callback,
                                 void *media_user_data)
{
    sip_server_handlers_t handlers;

    memset(&handlers, 0, sizeof(handlers));
    handlers.on_media = media_callback;
    handlers.user_data = media_user_data;

    return sip_server_run_with_handlers(config, stop_flag, &handlers);
}

int sip_server_run(const app_config_t *config, volatile sig_atomic_t *stop_flag)
{
    return sip_server_run_with_handlers(config, stop_flag, NULL);
}
