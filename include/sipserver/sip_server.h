#ifndef SIPSERVER_SIP_SERVER_H
#define SIPSERVER_SIP_SERVER_H

#include "sipserver/streamer.h"

/* 对外暴露的关键信令事件类型。 */
typedef enum {
    /* 收到对端 INVITE 请求后触发。
     * 该事件表示有新的呼叫建立请求到达，服务端已经完成基础报文解析，
     * 上层通常在这个阶段结合 on_invite 回调决定是否接听、如何生成 SDP Answer。
     */
    SIP_SIGNAL_INVITE_RECEIVED = 0,
    /* 收到对端 ACK 请求后触发。
     * 该事件通常出现在本端已经发送 200 OK 之后，表示对端确认了最终应答，
     * 媒体会话从这个时刻起可以认为正式建立，上层可据此启动采集、编码或转发逻辑。
     */
    SIP_SIGNAL_ACK_RECEIVED = 1,
    /* 收到对端 BYE 请求后触发。
     * 该事件表示当前会话被对端主动挂断，上层应在这里停止推流、回收会话相关资源，
     * 并准备处理随后的会话终止通知。
     */
    SIP_SIGNAL_BYE_RECEIVED = 2,
    /* 收到对端 OPTIONS 探测请求后触发。
     * 该事件常用于连通性探测或能力查询，不代表呼叫建立，
     * 上层通常只需要记录日志、统计访问或补充自定义能力信息。
     */
    SIP_SIGNAL_OPTIONS_RECEIVED = 3,
    /* 收到对端 REGISTER 请求后触发。
     * 该事件表示对端尝试进行注册或保活；当前工程里它更多用于观测信令流量，
     * 若后续扩展注册鉴权、设备在线管理或位置服务，可在这里接入上层逻辑。
     */
    SIP_SIGNAL_REGISTER_RECEIVED = 4,
    /* 本端向对端发出 SIP 响应后触发。
     * 无论是 1xx 临时响应、2xx 成功响应还是 4xx/5xx 失败响应，
     * 发送完成后都会通过该事件通知上层，便于记录最终返回码和原因短语。
     */
    SIP_SIGNAL_RESPONSE_SENT = 5,
    /* 对话建立后触发。
     * 一般发生在 INVITE 成功协商、媒体参数已经确定之后，
     * 回调中会携带当前会话对应的 streamer，可用于保存会话句柄并开始投递音视频帧。
     */
    SIP_SIGNAL_DIALOG_ESTABLISHED = 6,
    /* 对话结束后触发。
     * 可能由收到 BYE、处理失败或内部清理流程引起，
     * 上层应在这里清空当前会话句柄、停止业务线程，并避免继续向已失效的 streamer 推流。
     */
    SIP_SIGNAL_DIALOG_TERMINATED = 7
} sip_signal_type_t;

/* 通用 SIP 事件通知参数。 */
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

/* INVITE/Offer 解析结果，供上层决定是否接听。 */
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

/* 上层对 INVITE 的处理结果。 */
typedef struct {
    int accept;
    int send_ringing;
    int status_code;
    char reason_phrase[64];
    char answer_sdp[4096];
    streamer_session_params_t media;
} sip_invite_response_t;

/* 普通信令事件回调。 */
typedef void (*sip_signal_callback_t)(const sip_signal_event_t *event, void *user_data);
/* INVITE 回调：由上层决定是否接听并生成应答。 */
typedef int (*sip_invite_callback_t)(const sip_invite_event_t *event,
                                     sip_invite_response_t *response,
                                     void *user_data);

/* SIP 服务端可挂接的回调集合。 */
typedef struct {
    sip_signal_callback_t on_signal;
    sip_invite_callback_t on_invite;
    streamer_receive_callback_t on_media;
    void *user_data;
} sip_server_handlers_t;

/* 使用默认行为运行 SIP 服务。 */
int sip_server_run(const app_config_t *config, const volatile int *stop_requested);
/* 使用自定义媒体回调运行 SIP 服务。 */
int sip_server_run_with_callback(const app_config_t *config,
                                 const volatile int *stop_requested,
                                 streamer_receive_callback_t media_callback,
                                 void *media_user_data);
/* 使用完整回调集合运行 SIP 服务。 */
int sip_server_run_with_handlers(const app_config_t *config,
                                 const volatile int *stop_requested,
                                 const sip_server_handlers_t *handlers);

#endif
