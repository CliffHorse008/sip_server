#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "sip_server.h"

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int signum)
{
    (void) signum;
    g_stop = 1;
}

int main(int argc, char **argv)
{
    app_config_t config;
    int parse_rc;

    srand((unsigned int) time(NULL));

    parse_rc = config_parse(&config, argc, argv);
    if (parse_rc > 0) {
        return 0;
    }
    if (parse_rc < 0) {
        config_print_usage(stderr, argv[0]);
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    fprintf(stdout,
            "sipserver starting on %s:%u, media=%s, audio_codec=%s\n",
            config.bind_ip,
            config.sip_port,
            config.media_ip,
            config_audio_codec_name(config.audio_codec));

    return sip_server_run(&config, &g_stop);
}
