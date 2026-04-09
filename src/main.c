#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "sip_server.h"

#define G711A_SAMPLES_PER_PACKET 160U

static volatile sig_atomic_t g_stop = 0;

typedef struct {
    size_t offset;
    size_t size;
} h264_access_unit_t;

typedef struct {
    unsigned char *video_blob;
    size_t video_blob_size;
    h264_access_unit_t *video_frames;
    size_t video_frame_count;
    unsigned char *audio_blob;
    size_t audio_blob_size;
    pthread_t audio_thread;
    pthread_t video_thread;
    int audio_thread_running;
    int video_thread_running;
} sample_demo_media_t;

typedef struct {
    const app_config_t *config;
    pthread_mutex_t mutex;
    streamer_t *streamer;
    unsigned int stream_generation;
    int enable_demo_source;
    sample_demo_media_t demo_media;
} sample_sdk_t;

static void handle_signal(int signum)
{
    (void) signum;
    g_stop = 1;
}

static void sleep_ns(long long nanoseconds)
{
    struct timespec ts;

    if (nanoseconds <= 0) {
        return;
    }

    ts.tv_sec = (time_t) (nanoseconds / 1000000000LL);
    ts.tv_nsec = (long) (nanoseconds % 1000000000LL);
    while (nanosleep(&ts, &ts) != 0 && g_stop == 0) {
    }
}

static const char *signal_name(sip_signal_type_t type)
{
    switch (type) {
    case SIP_SIGNAL_INVITE_RECEIVED:
        return "INVITE_RECEIVED";
    case SIP_SIGNAL_ACK_RECEIVED:
        return "ACK_RECEIVED";
    case SIP_SIGNAL_BYE_RECEIVED:
        return "BYE_RECEIVED";
    case SIP_SIGNAL_OPTIONS_RECEIVED:
        return "OPTIONS_RECEIVED";
    case SIP_SIGNAL_REGISTER_RECEIVED:
        return "REGISTER_RECEIVED";
    case SIP_SIGNAL_RESPONSE_SENT:
        return "RESPONSE_SENT";
    case SIP_SIGNAL_DIALOG_ESTABLISHED:
        return "DIALOG_ESTABLISHED";
    case SIP_SIGNAL_DIALOG_TERMINATED:
        return "DIALOG_TERMINATED";
    default:
        return "UNKNOWN";
    }
}

static uint8_t default_audio_payload_type(audio_codec_t codec)
{
    return codec == AUDIO_CODEC_G711A ? 8 : 97;
}

static const char *default_transport(const char *transport)
{
    return transport != NULL && transport[0] != '\0' ? transport : "RTP/AVP";
}

static int env_flag_enabled(const char *name)
{
    const char *value = getenv(name);

    return value != NULL &&
           (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0);
}

static int read_entire_file(const char *path, unsigned char **buffer, size_t *size)
{
    FILE *stream;
    long file_size;
    unsigned char *blob;

    *buffer = NULL;
    *size = 0;

    stream = fopen(path, "rb");
    if (stream == NULL) {
        return -1;
    }

    if (fseek(stream, 0, SEEK_END) != 0) {
        fclose(stream);
        return -1;
    }

    file_size = ftell(stream);
    if (file_size < 0) {
        fclose(stream);
        return -1;
    }

    if (fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream);
        return -1;
    }

    blob = (unsigned char *) malloc((size_t) file_size);
    if (blob == NULL) {
        fclose(stream);
        return -1;
    }

    if (file_size > 0 && fread(blob, 1, (size_t) file_size, stream) != (size_t) file_size) {
        free(blob);
        fclose(stream);
        return -1;
    }

    fclose(stream);
    *buffer = blob;
    *size = (size_t) file_size;
    return 0;
}

static int find_start_code(const unsigned char *data,
                           size_t size,
                           size_t offset,
                           size_t *start_offset,
                           size_t *code_size)
{
    size_t index;

    if (size < 4 || offset >= size - 3) {
        return 0;
    }

    for (index = offset; index + 3 < size; ++index) {
        if (data[index] == 0x00 && data[index + 1] == 0x00) {
            if (data[index + 2] == 0x01) {
                *start_offset = index;
                *code_size = 3;
                return 1;
            }
            if (index + 4 < size && data[index + 2] == 0x00 && data[index + 3] == 0x01) {
                *start_offset = index;
                *code_size = 4;
                return 1;
            }
        }
    }

    return 0;
}

static int demo_append_h264_access_unit(sample_demo_media_t *demo_media, size_t offset, size_t size)
{
    h264_access_unit_t *grown;
    size_t new_count;

    if (size == 0) {
        return 0;
    }

    new_count = demo_media->video_frame_count + 1;
    grown = (h264_access_unit_t *) realloc(demo_media->video_frames, new_count * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }

    demo_media->video_frames = grown;
    demo_media->video_frames[demo_media->video_frame_count].offset = offset;
    demo_media->video_frames[demo_media->video_frame_count].size = size;
    demo_media->video_frame_count = new_count;
    return 0;
}

static int demo_build_h264_access_units(sample_demo_media_t *demo_media)
{
    size_t current_offset = 0;
    size_t search_offset = 0;
    int have_current = 0;

    while (1) {
        size_t start_offset;
        size_t code_size;
        size_t next_offset;
        size_t next_code_size;
        size_t nal_offset;
        unsigned char nal_type;

        if (!find_start_code(demo_media->video_blob,
                             demo_media->video_blob_size,
                             search_offset,
                             &start_offset,
                             &code_size)) {
            break;
        }

        nal_offset = start_offset + code_size;
        if (nal_offset >= demo_media->video_blob_size) {
            break;
        }

        nal_type = (unsigned char) (demo_media->video_blob[nal_offset] & 0x1F);

        if (nal_type == 9) {
            if (have_current && start_offset > current_offset) {
                if (demo_append_h264_access_unit(demo_media, current_offset, start_offset - current_offset) != 0) {
                    return -1;
                }
            }
            current_offset = start_offset;
            have_current = 1;
        } else if (!have_current) {
            current_offset = start_offset;
            have_current = 1;
        }

        if (!find_start_code(demo_media->video_blob,
                             demo_media->video_blob_size,
                             nal_offset,
                             &next_offset,
                             &next_code_size)) {
            (void) next_code_size;
            break;
        }

        search_offset = next_offset;
    }

    if (have_current && current_offset < demo_media->video_blob_size) {
        if (demo_append_h264_access_unit(demo_media,
                                         current_offset,
                                         demo_media->video_blob_size - current_offset) != 0) {
            return -1;
        }
    }

    return demo_media->video_frame_count > 0 ? 0 : -1;
}

static void sample_sdk_get_stream(sample_sdk_t *sdk, streamer_t **streamer, unsigned int *generation)
{
    pthread_mutex_lock(&sdk->mutex);
    *streamer = sdk->streamer;
    *generation = sdk->stream_generation;
    pthread_mutex_unlock(&sdk->mutex);
}

static void sample_sdk_set_stream(sample_sdk_t *sdk, streamer_t *streamer)
{
    pthread_mutex_lock(&sdk->mutex);
    sdk->streamer = streamer;
    ++sdk->stream_generation;
    pthread_mutex_unlock(&sdk->mutex);
}

static int sample_sdk_push_audio_frame(sample_sdk_t *sdk,
                                       const uint8_t *payload,
                                       size_t payload_size,
                                       uint32_t rtp_timestamp)
{
    streamer_audio_frame_t frame;
    streamer_t *streamer;
    unsigned int generation;

    sample_sdk_get_stream(sdk, &streamer, &generation);
    (void) generation;
    if (streamer == NULL) {
        return -1;
    }

    frame.data = payload;
    frame.size = payload_size;
    frame.timestamp = rtp_timestamp;
    return streamer_push_audio_frame(streamer, &frame);
}

static int sample_sdk_push_video_frame(sample_sdk_t *sdk,
                                       const uint8_t *access_unit,
                                       size_t access_unit_size,
                                       uint32_t rtp_timestamp)
{
    streamer_video_frame_t frame;
    streamer_t *streamer;
    unsigned int generation;

    sample_sdk_get_stream(sdk, &streamer, &generation);
    (void) generation;
    if (streamer == NULL) {
        return -1;
    }

    frame.data = access_unit;
    frame.size = access_unit_size;
    frame.timestamp = rtp_timestamp;
    return streamer_push_video_frame(streamer, &frame);
}

static int sample_sdk_build_answer(sample_sdk_t *sdk,
                                   const sip_invite_event_t *event,
                                   sip_invite_response_t *response)
{
    const app_config_t *config = sdk->config;
    streamer_sdp_plan_t plan;
    int audio_accepted = 0;
    int video_accepted = 0;

    memset(&plan, 0, sizeof(plan));
    memset(response, 0, sizeof(*response));

    if (event->offer_audio_present) {
        streamer_sdp_media_t *media = &plan.media[plan.media_count++];

        memset(media, 0, sizeof(*media));
        media->kind = STREAMER_MEDIA_AUDIO;
        media->payload_type = event->offer_audio_payload_type != 0
                                  ? event->offer_audio_payload_type
                                  : default_audio_payload_type(config->audio_codec);
        media->direction = STREAMER_DIRECTION_SENDRECV;
        snprintf(media->transport, sizeof(media->transport), "%s", default_transport(event->offer_audio_transport));

        if (config->audio_codec == AUDIO_CODEC_G711A && event->offer_audio_port != 0 && event->offer_audio_payload_type != 0) {
            media->accepted = 1;
            audio_accepted = 1;
        }
    }

    if (event->offer_video_present) {
        streamer_sdp_media_t *media = &plan.media[plan.media_count++];

        memset(media, 0, sizeof(*media));
        media->kind = STREAMER_MEDIA_VIDEO;
        media->payload_type = event->offer_video_payload_type != 0 ? event->offer_video_payload_type : 96;
        media->direction = STREAMER_DIRECTION_SENDRECV;
        snprintf(media->transport, sizeof(media->transport), "%s", default_transport(event->offer_video_transport));

        if (event->offer_video_port != 0 && event->offer_video_payload_type != 0) {
            media->accepted = 1;
            video_accepted = 1;
        }
    }

    if (!audio_accepted && !video_accepted) {
        response->accept = 0;
        response->status_code = 488;
        snprintf(response->reason_phrase, sizeof(response->reason_phrase), "%s", "Not Acceptable Here");
        return 0;
    }

    response->accept = 1;
    response->send_ringing = 1;
    response->status_code = 200;
    snprintf(response->reason_phrase, sizeof(response->reason_phrase), "%s", "OK");
    snprintf(response->media.remote_ip,
             sizeof(response->media.remote_ip),
             "%s",
             event->offer_connection_ip != NULL && event->offer_connection_ip[0] != '\0'
                 ? event->offer_connection_ip
                 : event->source_ip);
    response->media.remote_ip_alt[0] = '\0';
    response->media.audio_port = audio_accepted ? event->offer_audio_port : 0;
    response->media.video_port = video_accepted ? event->offer_video_port : 0;
    response->media.audio_payload_type = audio_accepted ? event->offer_audio_payload_type : 0;
    response->media.video_payload_type = video_accepted ? event->offer_video_payload_type : 0;
    response->media.audio_codec = config->audio_codec;
    response->media.video_enabled = video_accepted;
    response->media.live_input_only = sdk->enable_demo_source;

    return streamer_build_sdp(event->streamer, &plan, response->answer_sdp, sizeof(response->answer_sdp));
}

static void sample_sdk_on_signal(const sip_signal_event_t *event, void *user_data)
{
    sample_sdk_t *sdk = (sample_sdk_t *) user_data;

    if (event->type == SIP_SIGNAL_DIALOG_ESTABLISHED) {
        sample_sdk_set_stream(sdk, event->streamer);
    } else if (event->type == SIP_SIGNAL_DIALOG_TERMINATED) {
        sample_sdk_set_stream(sdk, NULL);
    }

    if (event->type == SIP_SIGNAL_RESPONSE_SENT) {
        fprintf(stdout,
                "signal %s call_id=%s status=%d reason=%s\n",
                signal_name(event->type),
                event->call_id != NULL ? event->call_id : "-",
                event->status_code,
                event->reason_phrase != NULL ? event->reason_phrase : "-");
        return;
    }

    fprintf(stdout,
            "signal %s call_id=%s method=%s from=%s source=%s:%u\n",
            signal_name(event->type),
            event->call_id != NULL ? event->call_id : "-",
            event->method != NULL ? event->method : "-",
            event->from != NULL ? event->from : "-",
            event->source_ip != NULL ? event->source_ip : "-",
            event->source_port);
}

static int sample_sdk_on_invite(const sip_invite_event_t *event,
                                sip_invite_response_t *response,
                                void *user_data)
{
    sample_sdk_t *sdk = (sample_sdk_t *) user_data;

    fprintf(stdout,
            "invite offer ip=%s audio_present=%d audio_port=%u audio_pt=%u video_present=%d video_port=%u video_pt=%u\n",
            event->offer_connection_ip != NULL ? event->offer_connection_ip : event->source_ip,
            event->offer_audio_present,
            event->offer_audio_port,
            event->offer_audio_payload_type,
            event->offer_video_present,
            event->offer_video_port,
            event->offer_video_payload_type);

    return sample_sdk_build_answer(sdk, event, response);
}

static void sample_sdk_on_media(const streamer_rtp_packet_t *packet, void *user_data)
{
    (void) user_data;

    fprintf(stdout,
            "media rx kind=%s pt=%u seq=%u ts=%u size=%zu from=%s:%u\n",
            packet->kind == STREAMER_MEDIA_AUDIO ? "audio" : "video",
            packet->payload_type,
            packet->sequence,
            packet->timestamp,
            packet->payload_size,
            packet->source_ip != NULL ? packet->source_ip : "-",
            packet->source_port);
}

static int sample_demo_media_load(sample_sdk_t *sdk)
{
    sample_demo_media_t *demo_media = &sdk->demo_media;

    if (!sdk->enable_demo_source) {
        return 0;
    }

    if (read_entire_file(sdk->config->video_path, &demo_media->video_blob, &demo_media->video_blob_size) == 0) {
        if (demo_build_h264_access_units(demo_media) != 0) {
            fprintf(stderr, "failed to parse H264 access units from %s\n", sdk->config->video_path);
            free(demo_media->video_blob);
            demo_media->video_blob = NULL;
            demo_media->video_blob_size = 0;
            free(demo_media->video_frames);
            demo_media->video_frames = NULL;
            demo_media->video_frame_count = 0;
        }
    } else {
        fprintf(stderr, "failed to load video file: %s\n", sdk->config->video_path);
    }

    if (sdk->config->audio_codec == AUDIO_CODEC_G711A) {
        if (read_entire_file(sdk->config->g711a_path, &demo_media->audio_blob, &demo_media->audio_blob_size) != 0) {
            fprintf(stderr, "failed to load G711A file: %s\n", sdk->config->g711a_path);
        }
    }

    fprintf(stdout,
            "demo media loaded video_frames=%zu audio_bytes=%zu\n",
            demo_media->video_frame_count,
            demo_media->audio_blob_size);
    return 0;
}

static void *sample_demo_audio_thread_main(void *opaque)
{
    sample_sdk_t *sdk = (sample_sdk_t *) opaque;
    sample_demo_media_t *demo_media = &sdk->demo_media;
    unsigned int generation = 0;
    size_t offset = 0;
    uint32_t timestamp = 0;
    int announced = 0;

    while (g_stop == 0) {
        streamer_t *streamer;

        sample_sdk_get_stream(sdk, &streamer, &generation);
        if (streamer == NULL || demo_media->audio_blob == NULL || demo_media->audio_blob_size == 0) {
            announced = 0;
            sleep_ns(20000000LL);
            continue;
        }

        if (!announced) {
            fprintf(stdout, "demo audio push started\n");
            announced = 1;
        }

        if (offset >= demo_media->audio_blob_size) {
            offset = 0;
        }

        {
            size_t remaining = demo_media->audio_blob_size - offset;
            size_t chunk = remaining >= G711A_SAMPLES_PER_PACKET ? G711A_SAMPLES_PER_PACKET : remaining;

            if (chunk == 0) {
                offset = 0;
                continue;
            }

            if (sample_sdk_push_audio_frame(sdk, demo_media->audio_blob + offset, chunk, timestamp) == 0) {
                offset += chunk;
                timestamp += (uint32_t) chunk;
            }
        }

        sleep_ns(20000000LL);

        {
            streamer_t *current_streamer;
            unsigned int current_generation;

            sample_sdk_get_stream(sdk, &current_streamer, &current_generation);
            if (current_generation != generation) {
                offset = 0;
                timestamp = 0;
                announced = 0;
            }
        }
    }

    return NULL;
}

static void *sample_demo_video_thread_main(void *opaque)
{
    sample_sdk_t *sdk = (sample_sdk_t *) opaque;
    sample_demo_media_t *demo_media = &sdk->demo_media;
    unsigned int generation = 0;
    size_t frame_index = 0;
    uint32_t timestamp = 0;
    long long frame_interval_ns = (long long) (1000000000.0 / sdk->config->video_fps);
    uint32_t timestamp_step = (uint32_t) (90000.0 / sdk->config->video_fps);
    int announced = 0;

    if (frame_interval_ns <= 0) {
        frame_interval_ns = 33333333LL;
    }
    if (timestamp_step == 0) {
        timestamp_step = 3000;
    }

    while (g_stop == 0) {
        streamer_t *streamer;

        sample_sdk_get_stream(sdk, &streamer, &generation);
        if (streamer == NULL || demo_media->video_frames == NULL || demo_media->video_frame_count == 0) {
            announced = 0;
            sleep_ns(10000000LL);
            continue;
        }

        if (!announced) {
            fprintf(stdout, "demo video push started\n");
            announced = 1;
        }

        if (frame_index >= demo_media->video_frame_count) {
            frame_index = 0;
        }

        {
            const h264_access_unit_t *access_unit = &demo_media->video_frames[frame_index];

            if (sample_sdk_push_video_frame(sdk,
                                            demo_media->video_blob + access_unit->offset,
                                            access_unit->size,
                                            timestamp) == 0) {
                ++frame_index;
                timestamp += timestamp_step;
            }
        }

        sleep_ns(frame_interval_ns);

        {
            streamer_t *current_streamer;
            unsigned int current_generation;

            sample_sdk_get_stream(sdk, &current_streamer, &current_generation);
            if (current_generation != generation) {
                frame_index = 0;
                timestamp = 0;
                announced = 0;
            }
        }
    }

    return NULL;
}

static int sample_demo_media_start(sample_sdk_t *sdk)
{
    sample_demo_media_t *demo_media = &sdk->demo_media;

    if (demo_media->audio_blob != NULL && demo_media->audio_blob_size > 0) {
        if (pthread_create(&demo_media->audio_thread, NULL, sample_demo_audio_thread_main, sdk) != 0) {
            perror("pthread_create(audio-push)");
            return -1;
        }
        demo_media->audio_thread_running = 1;
    }

    if (demo_media->video_frames != NULL && demo_media->video_frame_count > 0) {
        if (pthread_create(&demo_media->video_thread, NULL, sample_demo_video_thread_main, sdk) != 0) {
            perror("pthread_create(video-push)");
            g_stop = 1;
            if (demo_media->audio_thread_running) {
                pthread_join(demo_media->audio_thread, NULL);
                demo_media->audio_thread_running = 0;
            }
            return -1;
        }
        demo_media->video_thread_running = 1;
    }

    return 0;
}

static void sample_demo_media_stop(sample_sdk_t *sdk)
{
    sample_demo_media_t *demo_media = &sdk->demo_media;

    if (demo_media->audio_thread_running) {
        pthread_join(demo_media->audio_thread, NULL);
        demo_media->audio_thread_running = 0;
    }
    if (demo_media->video_thread_running) {
        pthread_join(demo_media->video_thread, NULL);
        demo_media->video_thread_running = 0;
    }
}

static void sample_sdk_destroy(sample_sdk_t *sdk)
{
    sample_demo_media_t *demo_media = &sdk->demo_media;

    sample_sdk_set_stream(sdk, NULL);
    sample_demo_media_stop(sdk);
    free(demo_media->video_frames);
    free(demo_media->video_blob);
    free(demo_media->audio_blob);
    pthread_mutex_destroy(&sdk->mutex);
}

static int sample_sdk_init(sample_sdk_t *sdk, const app_config_t *config)
{
    memset(sdk, 0, sizeof(*sdk));
    sdk->config = config;
    sdk->enable_demo_source = env_flag_enabled("SIPSERVER_UPPER_PUSH_DEMO");

    if (pthread_mutex_init(&sdk->mutex, NULL) != 0) {
        perror("pthread_mutex_init");
        return -1;
    }

    if (sample_demo_media_load(sdk) != 0) {
        sample_sdk_destroy(sdk);
        return -1;
    }

    if (sdk->enable_demo_source && sample_demo_media_start(sdk) != 0) {
        sample_sdk_destroy(sdk);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    app_config_t config;
    sample_sdk_t sdk;
    sip_server_handlers_t handlers;
    int parse_rc;
    int run_rc;

    srand((unsigned int) time(NULL));

    parse_rc = config_parse(&config, argc, argv);
    if (parse_rc > 0) {
        return 0;
    }
    if (parse_rc < 0) {
        config_print_usage(stderr, argv[0]);
        return 1;
    }

    if (sample_sdk_init(&sdk, &config) != 0) {
        return 1;
    }

    memset(&handlers, 0, sizeof(handlers));
    handlers.on_signal = sample_sdk_on_signal;
    handlers.on_invite = sample_sdk_on_invite;
    handlers.on_media = sample_sdk_on_media;
    handlers.user_data = &sdk;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    fprintf(stdout,
            "sipserver starting on %s:%u, media=%s, audio_codec=%s, upper_push_demo=%s\n",
            config.bind_ip,
            config.sip_port,
            config.media_ip,
            config_audio_codec_name(config.audio_codec),
            sdk.enable_demo_source ? "on" : "off");

    run_rc = sip_server_run_with_handlers(&config, &g_stop, &handlers);
    g_stop = 1;
    sample_sdk_destroy(&sdk);
    return run_rc;
}
