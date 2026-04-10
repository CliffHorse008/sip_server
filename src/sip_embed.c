#include "sipserver/sip_embed.h"

#include <stdlib.h>

struct sip_embed_service {
    sip_session_service_t *session;
};

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

int sip_embed_service_run(sip_embed_service_t *service, volatile sig_atomic_t *stop_flag)
{
    if (service == NULL) {
        return -1;
    }

    return sip_session_service_run(service->session, stop_flag);
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
