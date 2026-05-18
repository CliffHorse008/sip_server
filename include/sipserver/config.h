#ifndef SIPSERVER_CONFIG_H
#define SIPSERVER_CONFIG_H

#include <stdint.h>
#include <stdio.h>

/* 支持的音频编码类型。 */
typedef enum {
    AUDIO_CODEC_AAC = 0,
    AUDIO_CODEC_G711A = 1
} audio_codec_t;

/* 支持的 SIP 信令传输方式。 */
typedef enum {
    SIP_TRANSPORT_UDP = 0,
    SIP_TRANSPORT_TCP = 1
} sip_transport_t;

/* 支持的 RTP 媒体传输方式。 */
typedef enum {
    RTP_TRANSPORT_UDP = 0,
    RTP_TRANSPORT_KCP = 1
} rtp_transport_t;

/* 由业务在运行期提供对外公布的媒体 IP。 */
typedef const char *(*media_ip_provider_t)(void *user_data);

/* 应用启动参数与运行期配置。 */
typedef struct {
    char bind_ip[64];
    char media_ip[64];
    media_ip_provider_t media_ip_provider;
    void *media_ip_provider_user_data;
    sip_transport_t sip_transport;
    rtp_transport_t rtp_transport;
    uint16_t sip_port;
    uint16_t sip_session_expires;
    uint16_t audio_port;
    uint16_t video_port;
    audio_codec_t audio_codec;
} app_config_t;

/* 填充默认配置。 */
void config_set_defaults(app_config_t *config);
/* 解析命令行参数；返回值含义见实现。 */
int config_parse(app_config_t *config, int argc, char **argv);
/* 打印命令行帮助。 */
void config_print_usage(FILE *stream, const char *program_name);
/* 注册运行期媒体 IP 提供器；传 NULL 表示恢复静态 media_ip。 */
void config_set_media_ip_provider(app_config_t *config, media_ip_provider_t provider, void *user_data);
/* 获取当前应对外公布的媒体 IP。 */
const char *config_get_media_ip(const app_config_t *config);
/* 返回音频编码名称字符串。 */
const char *config_audio_codec_name(audio_codec_t codec);
/* 返回 SIP 传输方式名称字符串。 */
const char *config_sip_transport_name(sip_transport_t transport);
/* 返回 RTP 传输方式名称字符串。 */
const char *config_rtp_transport_name(rtp_transport_t transport);

#endif
