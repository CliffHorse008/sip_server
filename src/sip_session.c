#include "sipserver/sip_session.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sip_session_service {
    const app_config_t *config;
    pthread_mutex_t mutex;
    streamer_t *streamer;
    unsigned int stream_generation;
    sip_session_callbacks_t callbacks;
};

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

static const char *default_transport_for_config(const app_config_t *config)
{
    return config->rtp_transport == RTP_TRANSPORT_KCP ? "KCP/RTP/AVP" : "RTP/AVP";
}

static const char *default_transport(const app_config_t *config, const char *transport)
{
    return transport != NULL && transport[0] != '\0' ? transport : default_transport_for_config(config);
}

static void sip_session_service_set_stream(sip_session_service_t *service, streamer_t *streamer)
{
    pthread_mutex_lock(&service->mutex);
    service->streamer = streamer;
    ++service->stream_generation;
    pthread_mutex_unlock(&service->mutex);
}

static int sip_session_service_build_answer(sip_session_service_t *service,
                                            const sip_invite_event_t *event,
                                            sip_invite_response_t *response)
{
    const app_config_t *config = service->config;
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
        snprintf(media->transport,
                 sizeof(media->transport),
                 "%s",
                 default_transport(config, event->offer_audio_transport));

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
        snprintf(media->transport,
                 sizeof(media->transport),
                 "%s",
                 default_transport(config, event->offer_video_transport));

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
    snprintf(response->media.audio_transport,
             sizeof(response->media.audio_transport),
             "%s",
             audio_accepted ? default_transport(config, event->offer_audio_transport) : "");
    snprintf(response->media.video_transport,
             sizeof(response->media.video_transport),
             "%s",
             video_accepted ? default_transport(config, event->offer_video_transport) : "");
    response->media.audio_port = audio_accepted ? event->offer_audio_port : 0;
    response->media.video_port = video_accepted ? event->offer_video_port : 0;
    response->media.audio_payload_type = audio_accepted ? event->offer_audio_payload_type : 0;
    response->media.video_payload_type = video_accepted ? event->offer_video_payload_type : 0;
    response->media.audio_codec = config->audio_codec;
    response->media.video_enabled = video_accepted;
    response->media.live_input_only = 1;

    return streamer_build_sdp(event->streamer, &plan, response->answer_sdp, sizeof(response->answer_sdp));
}

static void sip_session_on_signal(const sip_signal_event_t *event, void *user_data)
{
    sip_session_service_t *service = (sip_session_service_t *) user_data;

    if (event->type == SIP_SIGNAL_DIALOG_ESTABLISHED) {
        sip_session_service_set_stream(service, event->streamer);
    } else if (event->type == SIP_SIGNAL_DIALOG_TERMINATED) {
        sip_session_service_set_stream(service, NULL);
    }

    if (event->type == SIP_SIGNAL_RESPONSE_SENT) {
        fprintf(stdout,
                "signal %s call_id=%s status=%d reason=%s\n",
                signal_name(event->type),
                event->call_id != NULL ? event->call_id : "-",
                event->status_code,
                event->reason_phrase != NULL ? event->reason_phrase : "-");
    } else {
        fprintf(stdout,
                "signal %s call_id=%s method=%s from=%s source=%s:%u\n",
                signal_name(event->type),
                event->call_id != NULL ? event->call_id : "-",
                event->method != NULL ? event->method : "-",
                event->from != NULL ? event->from : "-",
                event->source_ip != NULL ? event->source_ip : "-",
                event->source_port);
    }

    if (service->callbacks.on_signal != NULL) {
        service->callbacks.on_signal(event, service->callbacks.user_data);
    }
}

static int sip_session_on_invite(const sip_invite_event_t *event,
                                 sip_invite_response_t *response,
                                 void *user_data)
{
    sip_session_service_t *service = (sip_session_service_t *) user_data;

    fprintf(stdout,
            "invite offer ip=%s audio_present=%d audio_port=%u audio_pt=%u video_present=%d video_port=%u video_pt=%u\n",
            event->offer_connection_ip != NULL ? event->offer_connection_ip : event->source_ip,
            event->offer_audio_present,
            event->offer_audio_port,
            event->offer_audio_payload_type,
            event->offer_video_present,
            event->offer_video_port,
            event->offer_video_payload_type);

    if (service->callbacks.on_invite != NULL) {
        return service->callbacks.on_invite(event, response, service->callbacks.user_data);
    }

    return sip_session_service_build_answer(service, event, response);
}

static void sip_session_on_media(const streamer_rtp_packet_t *packet, void *user_data)
{
    sip_session_service_t *service = (sip_session_service_t *) user_data;

    if (service->callbacks.on_media != NULL) {
        service->callbacks.on_media(packet, service->callbacks.user_data);
    }
}

sip_session_service_t *sip_session_service_create(const app_config_t *config)
{
    sip_session_service_t *service;

    if (config == NULL) {
        return NULL;
    }

    service = (sip_session_service_t *) calloc(1, sizeof(*service));
    if (service == NULL) {
        return NULL;
    }

    service->config = config;
    if (pthread_mutex_init(&service->mutex, NULL) != 0) {
        free(service);
        return NULL;
    }

    return service;
}

void sip_session_service_destroy(sip_session_service_t *service)
{
    if (service == NULL) {
        return;
    }

    sip_session_service_set_stream(service, NULL);
    pthread_mutex_destroy(&service->mutex);
    free(service);
}

void sip_session_service_set_callbacks(sip_session_service_t *service, const sip_session_callbacks_t *callbacks)
{
    if (service == NULL) {
        return;
    }

    if (callbacks == NULL) {
        memset(&service->callbacks, 0, sizeof(service->callbacks));
        return;
    }

    service->callbacks = *callbacks;
}

int sip_session_service_run(sip_session_service_t *service, volatile sig_atomic_t *stop_flag)
{
    sip_server_handlers_t handlers;

    if (service == NULL || stop_flag == NULL) {
        return -1;
    }

    memset(&handlers, 0, sizeof(handlers));
    handlers.on_signal = sip_session_on_signal;
    handlers.on_invite = sip_session_on_invite;
    handlers.on_media = sip_session_on_media;
    handlers.user_data = service;

    return sip_server_run_with_handlers(service->config, stop_flag, &handlers);
}

streamer_t *sip_session_service_get_stream(sip_session_service_t *service, unsigned int *generation)
{
    streamer_t *streamer = NULL;
    unsigned int current_generation = 0;

    if (service == NULL) {
        if (generation != NULL) {
            *generation = 0;
        }
        return NULL;
    }

    pthread_mutex_lock(&service->mutex);
    streamer = service->streamer;
    current_generation = service->stream_generation;
    pthread_mutex_unlock(&service->mutex);

    if (generation != NULL) {
        *generation = current_generation;
    }

    return streamer;
}

void sip_session_service_get_stream_state(sip_session_service_t *service, int *stream_active, unsigned int *generation)
{
    streamer_t *streamer = sip_session_service_get_stream(service, generation);

    if (stream_active != NULL) {
        *stream_active = streamer != NULL;
    }
}
