#ifndef SIPSERVER_SIP_EMBED_H
#define SIPSERVER_SIP_EMBED_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "sipserver/sip_session.h"

typedef sip_session_callbacks_t sip_embed_callbacks_t;

/* 嵌入式服务实例的前置声明。 */
typedef struct sip_embed_service sip_embed_service_t;

/* 读取 sipserver 库的编译时间。 */
const char *sip_embed_build_time(void);
/* 创建嵌入式服务实例。 */
sip_embed_service_t *sip_embed_service_create(const app_config_t *config);
/* 销毁嵌入式服务实例。 */
void sip_embed_service_destroy(sip_embed_service_t *service);
/* 设置可选的宿主回调。 */
void sip_embed_service_set_callbacks(sip_embed_service_t *service, const sip_embed_callbacks_t *callbacks);
/* 运行 SIP 会话层。 */
int sip_embed_service_run(sip_embed_service_t *service, volatile sig_atomic_t *stop_flag);
/* 读取当前会话状态与代数，便于宿主同步推流状态。 */
void sip_embed_service_get_stream_state(sip_embed_service_t *service, int *stream_active, unsigned int *generation);
/* 向当前活动会话投递一帧音频。 */
int sip_embed_service_push_audio_frame(sip_embed_service_t *service,
                                       const uint8_t *payload,
                                       size_t payload_size,
                                       uint32_t rtp_timestamp);
/* 向当前活动会话投递一帧视频。 */
int sip_embed_service_push_video_frame(sip_embed_service_t *service,
                                       const uint8_t *access_unit,
                                       size_t access_unit_size,
                                       uint32_t rtp_timestamp);

#endif
