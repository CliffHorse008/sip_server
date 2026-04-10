#ifndef SIPSERVER_STREAMER_H
#define SIPSERVER_STREAMER_H

#include <stddef.h>
#include <stdint.h>

#include "sipserver/config.h"

#define STREAMER_SDP_MAX_MEDIA 8

/* 媒体发送/接收器的前置声明。 */
typedef struct streamer streamer_t;

/* RTP 媒体类型。 */
typedef enum {
    STREAMER_MEDIA_AUDIO = 0,
    STREAMER_MEDIA_VIDEO = 1
} streamer_media_kind_t;

/* SDP 中的媒体方向属性。 */
typedef enum {
    STREAMER_DIRECTION_SENDRECV = 0,
    STREAMER_DIRECTION_SENDONLY = 1,
    STREAMER_DIRECTION_RECVONLY = 2,
    STREAMER_DIRECTION_INACTIVE = 3
} streamer_direction_t;

/* 生成 SDP Answer 时单个媒体行的协商结果。 */
typedef struct {
    streamer_media_kind_t kind;
    int accepted;
    uint16_t port;
    uint8_t payload_type;
    char transport[32];
    streamer_direction_t direction;
} streamer_sdp_media_t;

/* 生成整份 SDP Answer 的媒体计划。 */
typedef struct {
    streamer_sdp_media_t media[STREAMER_SDP_MAX_MEDIA];
    size_t media_count;
} streamer_sdp_plan_t;

/* 收到的 RTP 包元数据。 */
typedef struct {
    streamer_media_kind_t kind;
    const uint8_t *payload;
    size_t payload_size;
    uint8_t payload_type;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
    const char *source_ip;
    uint16_t source_port;
} streamer_rtp_packet_t;

/* RTP 接收回调。 */
typedef void (*streamer_receive_callback_t)(const streamer_rtp_packet_t *packet, void *user_data);

/* 启动一条媒体会话所需的参数。 */
typedef struct {
    char remote_ip[64];
    char remote_ip_alt[64];
    uint16_t audio_port;
    uint16_t video_port;
    uint8_t audio_payload_type;
    uint8_t video_payload_type;
    audio_codec_t audio_codec;
    int video_enabled;
    int live_input_only;
} streamer_session_params_t;

/* 上层投递的音频帧。 */
typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t timestamp;
} streamer_audio_frame_t;

/* 上层投递的视频帧。 */
typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t timestamp;
} streamer_video_frame_t;

/* 创建媒体收发器。 */
streamer_t *streamer_create(const app_config_t *config);
/* 销毁媒体收发器。 */
void streamer_destroy(streamer_t *streamer);
/* 注册 RTP 接收回调。 */
void streamer_set_receive_callback(streamer_t *streamer,
                                   streamer_receive_callback_t callback,
                                   void *user_data);
/* 按协商结果启动媒体会话。 */
int streamer_start(streamer_t *streamer, const streamer_session_params_t *params);
/* 线程安全地推送音频帧。 */
int streamer_push_audio_frame(streamer_t *streamer, const streamer_audio_frame_t *frame);
/* 线程安全地推送视频帧。 */
int streamer_push_video_frame(streamer_t *streamer, const streamer_video_frame_t *frame);
/* 停止媒体发送与接收。 */
void streamer_stop(streamer_t *streamer);
/* 根据协商计划构造 SDP Answer。 */
int streamer_build_sdp(const streamer_t *streamer,
                       const streamer_sdp_plan_t *plan,
                       char *buffer,
                       size_t buffer_size);

#endif
