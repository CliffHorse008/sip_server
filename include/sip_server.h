#ifndef SIP_SERVER_H
#define SIP_SERVER_H

#include <signal.h>

#include "streamer.h"

int sip_server_run(const app_config_t *config, volatile sig_atomic_t *stop_flag);
int sip_server_run_with_callback(const app_config_t *config,
                                 volatile sig_atomic_t *stop_flag,
                                 streamer_receive_callback_t media_callback,
                                 void *media_user_data);

#endif
