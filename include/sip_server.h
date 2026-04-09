#ifndef SIP_SERVER_H
#define SIP_SERVER_H

#include <signal.h>

#include "streamer.h"

typedef enum {
    SIP_SIGNAL_INVITE_RECEIVED = 0,
    SIP_SIGNAL_ACK_RECEIVED = 1,
    SIP_SIGNAL_BYE_RECEIVED = 2,
    SIP_SIGNAL_OPTIONS_RECEIVED = 3,
    SIP_SIGNAL_REGISTER_RECEIVED = 4,
    SIP_SIGNAL_RESPONSE_SENT = 5,
    SIP_SIGNAL_DIALOG_ESTABLISHED = 6,
    SIP_SIGNAL_DIALOG_TERMINATED = 7
} sip_signal_type_t;

typedef struct {
    sip_signal_type_t type;
    streamer_t *streamer;
    const char *method;
    int status_code;
    const char *reason_phrase;
    const char *call_id;
    const char *from;
    const char *to;
    const char *body;
    const char *source_ip;
    uint16_t source_port;
} sip_signal_event_t;

typedef struct {
    streamer_t *streamer;
    const char *raw_message;
    const char *offer_sdp;
    const char *call_id;
    const char *from;
    const char *to;
    const char *via;
    const char *source_ip;
    uint16_t source_port;
    const char *offer_connection_ip;
    int offer_audio_present;
    int offer_video_present;
    const char *offer_audio_transport;
    const char *offer_video_transport;
    uint16_t offer_audio_port;
    uint16_t offer_video_port;
    uint8_t offer_audio_payload_type;
    uint8_t offer_video_payload_type;
} sip_invite_event_t;

typedef struct {
    int accept;
    int send_ringing;
    int status_code;
    char reason_phrase[64];
    char answer_sdp[4096];
    streamer_session_params_t media;
} sip_invite_response_t;

typedef void (*sip_signal_callback_t)(const sip_signal_event_t *event, void *user_data);
typedef int (*sip_invite_callback_t)(const sip_invite_event_t *event,
                                     sip_invite_response_t *response,
                                     void *user_data);

typedef struct {
    sip_signal_callback_t on_signal;
    sip_invite_callback_t on_invite;
    streamer_receive_callback_t on_media;
    void *user_data;
} sip_server_handlers_t;

int sip_server_run(const app_config_t *config, volatile sig_atomic_t *stop_flag);
int sip_server_run_with_callback(const app_config_t *config,
                                 volatile sig_atomic_t *stop_flag,
                                 streamer_receive_callback_t media_callback,
                                 void *media_user_data);
int sip_server_run_with_handlers(const app_config_t *config,
                                 volatile sig_atomic_t *stop_flag,
                                 const sip_server_handlers_t *handlers);

#endif
