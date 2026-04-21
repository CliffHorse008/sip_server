#ifndef SIPSERVER_SIP_SESSION_H
#define SIPSERVER_SIP_SESSION_H

#include "sipserver/sip_server.h"

/* SIP 会话层对外回调。
 * 如果未设置 on_invite，则使用库内默认的最小接听策略。
 */
typedef struct {
    sip_signal_callback_t on_signal;
    sip_invite_callback_t on_invite;
    streamer_receive_callback_t on_media;
    void *user_data;
} sip_session_callbacks_t;

/* SIP 会话服务实例的前置声明。 */
typedef struct sip_session_service sip_session_service_t;

/* 创建 SIP 会话服务实例。 */
sip_session_service_t *sip_session_service_create(const app_config_t *config);
/* 销毁 SIP 会话服务实例。 */
void sip_session_service_destroy(sip_session_service_t *service);
/* 设置可选的宿主回调。 */
void sip_session_service_set_callbacks(sip_session_service_t *service, const sip_session_callbacks_t *callbacks);
/* 请求 SIP 会话服务停止。 */
void sip_session_service_stop(sip_session_service_t *service);
/* 查询 SIP 会话服务是否已收到停止请求。 */
int sip_session_service_stop_requested(sip_session_service_t *service);
/* 运行 SIP 会话服务。 */
int sip_session_service_run(sip_session_service_t *service);
/* 获取当前活动会话对应的 streamer 及其代数。 */
streamer_t *sip_session_service_get_stream(sip_session_service_t *service, unsigned int *generation);
/* 读取当前会话状态与代数。 */
void sip_session_service_get_stream_state(sip_session_service_t *service, int *stream_active, unsigned int *generation);

#endif
