#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdio.h>

typedef enum {
    AUDIO_CODEC_AAC = 0,
    AUDIO_CODEC_G711A = 1
} audio_codec_t;

typedef struct {
    char bind_ip[64];
    char media_ip[64];
    uint16_t sip_port;
    uint16_t audio_port;
    uint16_t video_port;
    double video_fps;
    audio_codec_t audio_codec;
    char video_path[256];
    char aac_path[256];
    char g711a_path[256];
} app_config_t;

void config_set_defaults(app_config_t *config);
int config_parse(app_config_t *config, int argc, char **argv);
void config_print_usage(FILE *stream, const char *program_name);
const char *config_audio_codec_name(audio_codec_t codec);

#endif
