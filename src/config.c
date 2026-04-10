#include "sipserver/config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int parse_u16(const char *value, uint16_t *out)
{
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);

    if (value[0] == '\0' || end == NULL || *end != '\0' || parsed > 65535UL) {
        return -1;
    }

    *out = (uint16_t) parsed;
    return 0;
}

static int parse_double_value(const char *value, double *out)
{
    char *end = NULL;
    double parsed = strtod(value, &end);

    if (value[0] == '\0' || end == NULL || *end != '\0') {
        return -1;
    }

    *out = parsed;
    return 0;
}

static int parse_sip_transport(const char *value, sip_transport_t *out)
{
    if (strcmp(value, "udp") == 0) {
        *out = SIP_TRANSPORT_UDP;
        return 0;
    }
    if (strcmp(value, "tcp") == 0) {
        *out = SIP_TRANSPORT_TCP;
        return 0;
    }

    return -1;
}

void config_set_defaults(app_config_t *config)
{
    memset(config, 0, sizeof(*config));

    snprintf(config->bind_ip, sizeof(config->bind_ip), "0.0.0.0");
    snprintf(config->media_ip, sizeof(config->media_ip), "127.0.0.1");
    config->sip_transport = SIP_TRANSPORT_UDP;
    config->sip_port = 5060;
    config->audio_port = 5004;
    config->video_port = 5006;
    config->video_fps = 30.0;
    config->audio_codec = AUDIO_CODEC_AAC;
    snprintf(config->video_path, sizeof(config->video_path), "test_media/video.h264");
    snprintf(config->aac_path, sizeof(config->aac_path), "test_media/audio.aac");
    snprintf(config->g711a_path, sizeof(config->g711a_path), "test_media/audio.g711a");
}

const char *config_audio_codec_name(audio_codec_t codec)
{
    return codec == AUDIO_CODEC_G711A ? "g711a" : "aac";
}

const char *config_sip_transport_name(sip_transport_t transport)
{
    return transport == SIP_TRANSPORT_TCP ? "tcp" : "udp";
}

void config_print_usage(FILE *stream, const char *program_name)
{
    fprintf(stream,
            "Usage: %s [options]\n"
            "  --bind-ip <ip>        SIP/RTP bind address, default 0.0.0.0\n"
            "  --media-ip <ip>       SDP announced media address, default 127.0.0.1\n"
            "  --sip-port <port>     SIP listen port, default 5060\n"
            "  --sip-transport <t>   udp or tcp, default udp\n"
            "  --audio-port <port>   Local RTP audio port, default 5004\n"
            "  --video-port <port>   Local RTP video port, default 5006\n"
            "  --video-fps <fps>     H264 pacing FPS, default 30.0\n"
            "  --audio-codec <name>  aac or g711a, default aac\n"
            "  --video-file <path>   H264 Annex-B bitstream path\n"
            "  --aac-file <path>     AAC ADTS bitstream path\n"
            "  --g711a-file <path>   G711A raw payload path\n"
            "  --help                Show this help\n",
            program_name);
}

int config_parse(app_config_t *config, int argc, char **argv)
{
    int index;

    config_set_defaults(config);

    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0) {
            config_print_usage(stdout, argv[0]);
            return 1;
        }

        if (index + 1 >= argc) {
            fprintf(stderr, "missing value for option: %s\n", arg);
            return -1;
        }

        if (strcmp(arg, "--bind-ip") == 0) {
            snprintf(config->bind_ip, sizeof(config->bind_ip), "%s", argv[++index]);
        } else if (strcmp(arg, "--media-ip") == 0) {
            snprintf(config->media_ip, sizeof(config->media_ip), "%s", argv[++index]);
        } else if (strcmp(arg, "--sip-port") == 0) {
            if (parse_u16(argv[++index], &config->sip_port) != 0) {
                fprintf(stderr, "invalid sip port\n");
                return -1;
            }
        } else if (strcmp(arg, "--sip-transport") == 0) {
            if (parse_sip_transport(argv[++index], &config->sip_transport) != 0) {
                fprintf(stderr, "invalid sip transport\n");
                return -1;
            }
        } else if (strcmp(arg, "--audio-port") == 0) {
            if (parse_u16(argv[++index], &config->audio_port) != 0) {
                fprintf(stderr, "invalid audio port\n");
                return -1;
            }
        } else if (strcmp(arg, "--video-port") == 0) {
            if (parse_u16(argv[++index], &config->video_port) != 0) {
                fprintf(stderr, "invalid video port\n");
                return -1;
            }
        } else if (strcmp(arg, "--video-fps") == 0) {
            if (parse_double_value(argv[++index], &config->video_fps) != 0 || config->video_fps <= 0.0) {
                fprintf(stderr, "invalid video fps\n");
                return -1;
            }
        } else if (strcmp(arg, "--audio-codec") == 0) {
            const char *codec = argv[++index];
            if (strcmp(codec, "aac") == 0) {
                config->audio_codec = AUDIO_CODEC_AAC;
            } else if (strcmp(codec, "g711a") == 0) {
                config->audio_codec = AUDIO_CODEC_G711A;
            } else {
                fprintf(stderr, "unsupported audio codec: %s\n", codec);
                return -1;
            }
        } else if (strcmp(arg, "--video-file") == 0) {
            snprintf(config->video_path, sizeof(config->video_path), "%s", argv[++index]);
        } else if (strcmp(arg, "--aac-file") == 0) {
            snprintf(config->aac_path, sizeof(config->aac_path), "%s", argv[++index]);
        } else if (strcmp(arg, "--g711a-file") == 0) {
            snprintf(config->g711a_path, sizeof(config->g711a_path), "%s", argv[++index]);
        } else {
            fprintf(stderr, "unknown option: %s\n", arg);
            return -1;
        }
    }

    return 0;
}
