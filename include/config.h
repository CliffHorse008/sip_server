#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdio.h>

/* 支持的音频编码类型。 */
typedef enum {
    AUDIO_CODEC_AAC = 0,
    AUDIO_CODEC_G711A = 1
} audio_codec_t;

/* 应用启动参数与运行期配置。 */
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

/* 填充默认配置。 */
void config_set_defaults(app_config_t *config);
/* 解析命令行参数；返回值含义见实现。 */
int config_parse(app_config_t *config, int argc, char **argv);
/* 打印命令行帮助。 */
void config_print_usage(FILE *stream, const char *program_name);
/* 返回音频编码名称字符串。 */
const char *config_audio_codec_name(audio_codec_t codec);

#endif
