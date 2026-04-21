#include "sipserver/sip_embed.h"

#include <stdio.h>
#include <stdlib.h>

static int sip_embed_build_year(void)
{
    return (__DATE__[7] - '0') * 1000 + (__DATE__[8] - '0') * 100 + (__DATE__[9] - '0') * 10 + (__DATE__[10] - '0');
}

static int sip_embed_build_day(void)
{
    if (__DATE__[4] == ' ') {
        return __DATE__[5] - '0';
    }

    return (__DATE__[4] - '0') * 10 + (__DATE__[5] - '0');
}

static int sip_embed_build_month(void)
{
    if (__DATE__[0] == 'J' && __DATE__[1] == 'a' && __DATE__[2] == 'n') {
        return 1;
    }
    if (__DATE__[0] == 'F') {
        return 2;
    }
    if (__DATE__[0] == 'M' && __DATE__[2] == 'r') {
        return 3;
    }
    if (__DATE__[0] == 'A' && __DATE__[1] == 'p') {
        return 4;
    }
    if (__DATE__[0] == 'M' && __DATE__[2] == 'y') {
        return 5;
    }
    if (__DATE__[0] == 'J' && __DATE__[2] == 'n') {
        return 6;
    }
    if (__DATE__[0] == 'J' && __DATE__[2] == 'l') {
        return 7;
    }
    if (__DATE__[0] == 'A' && __DATE__[1] == 'u') {
        return 8;
    }
    if (__DATE__[0] == 'S') {
        return 9;
    }
    if (__DATE__[0] == 'O') {
        return 10;
    }
    if (__DATE__[0] == 'N') {
        return 11;
    }
    return 12;
}

struct sip_embed_service {
    sip_session_service_t *session;
};

const char *sip_embed_build_time(void)
{
    _Thread_local static char build_time[64];

    snprintf(build_time,
             sizeof(build_time),
             "%04d年%02d月%02d日 %s",
             sip_embed_build_year(),
             sip_embed_build_month(),
             sip_embed_build_day(),
             __TIME__);
    return build_time;
}

sip_embed_service_t *sip_embed_service_create(const app_config_t *config)
{
    sip_embed_service_t *service;

    if (config == NULL) {
        return NULL;
    }

    service = (sip_embed_service_t *) calloc(1, sizeof(*service));
    if (service == NULL) {
        return NULL;
    }

    service->session = sip_session_service_create(config);
    if (service->session == NULL) {
        free(service);
        return NULL;
    }

    return service;
}

void sip_embed_service_destroy(sip_embed_service_t *service)
{
    if (service == NULL) {
        return;
    }

    sip_session_service_destroy(service->session);
    free(service);
}

void sip_embed_service_set_callbacks(sip_embed_service_t *service, const sip_embed_callbacks_t *callbacks)
{
    if (service == NULL) {
        return;
    }

    sip_session_service_set_callbacks(service->session, callbacks);
}

void sip_embed_service_stop(sip_embed_service_t *service)
{
    if (service == NULL) {
        return;
    }

    sip_session_service_stop(service->session);
}

int sip_embed_service_stop_requested(sip_embed_service_t *service)
{
    if (service == NULL) {
        return 1;
    }

    return sip_session_service_stop_requested(service->session);
}

int sip_embed_service_run(sip_embed_service_t *service)
{
    if (service == NULL) {
        return -1;
    }

    return sip_session_service_run(service->session);
}

void sip_embed_service_get_stream_state(sip_embed_service_t *service, int *stream_active, unsigned int *generation)
{
    if (service == NULL) {
        if (stream_active != NULL) {
            *stream_active = 0;
        }
        if (generation != NULL) {
            *generation = 0;
        }
        return;
    }

    sip_session_service_get_stream_state(service->session, stream_active, generation);
}

int sip_embed_service_audio_backpressure_high(sip_embed_service_t *service)
{
    streamer_t *streamer;
    unsigned int generation;

    if (service == NULL) {
        return 0;
    }

    streamer = sip_session_service_get_stream(service->session, &generation);
    (void) generation;
    if (streamer == NULL) {
        return 0;
    }

    return streamer_audio_backpressure_high(streamer);
}

int sip_embed_service_video_backpressure_high(sip_embed_service_t *service)
{
    streamer_t *streamer;
    unsigned int generation;

    if (service == NULL) {
        return 0;
    }

    streamer = sip_session_service_get_stream(service->session, &generation);
    (void) generation;
    if (streamer == NULL) {
        return 0;
    }

    return streamer_video_backpressure_high(streamer);
}

int sip_embed_service_push_audio_frame(sip_embed_service_t *service,
                                       const uint8_t *payload,
                                       size_t payload_size,
                                       uint32_t rtp_timestamp)
{
    streamer_audio_frame_t frame;
    streamer_t *streamer;
    unsigned int generation;

    if (service == NULL) {
        return -1;
    }

    streamer = sip_session_service_get_stream(service->session, &generation);
    (void) generation;
    if (streamer == NULL) {
        return -1;
    }

    frame.data = payload;
    frame.size = payload_size;
    frame.timestamp = rtp_timestamp;
    return streamer_push_audio_frame(streamer, &frame);
}

int sip_embed_service_push_video_frame(sip_embed_service_t *service,
                                       const uint8_t *access_unit,
                                       size_t access_unit_size,
                                       uint32_t rtp_timestamp)
{
    streamer_video_frame_t frame;
    streamer_t *streamer;
    unsigned int generation;

    if (service == NULL) {
        return -1;
    }

    streamer = sip_session_service_get_stream(service->session, &generation);
    (void) generation;
    if (streamer == NULL) {
        return -1;
    }

    frame.data = access_unit;
    frame.size = access_unit_size;
    frame.timestamp = rtp_timestamp;
    return streamer_push_video_frame(streamer, &frame);
}
