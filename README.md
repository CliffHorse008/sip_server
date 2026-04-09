# Minimal SIP Server

一个最小可编译的 SIP Server/RTP 发送/接收框架，使用 C 开发、CMake 构建，不依赖第三方运行库。工程目标是：

- SIP 信令走 UDP，默认监听 `5060`
- 媒体传输走 UDP/RTP
- 视频支持 H264
- 音频支持 AAC 和 G711A
- 测试素材来自当前目录的 `dlc_video.mp4`
- `ffmpeg` 仅用于离线生成测试码流，和 `sipserver` 运行时完全解耦

## 目录结构

```text
.
├── CMakeLists.txt
├── README.md
├── include/
├── scripts/
│   └── prepare_media.sh
├── src/
└── test_media/
```

## 功能边界

当前实现是“最小框架”，重点在工程骨架、媒体发送链路和 RTP 回调接入点：

- 支持 SIP UDP 基础方法：`OPTIONS`、`REGISTER`、`INVITE`、`ACK`、`BYE`
- 收到 `INVITE` 后解析对端 SDP 中的 `m=`、`rtpmap`、`fmtp`、方向属性和 RTP 端口
- 默认模式下会按 Offer 生成最小可用的 SDP Answer，并沿用对端的 payload type
- 收到 `ACK` 后启动音视频 RTP 发送线程
- 支持上层透传关键 SIP 事件
- 支持上层决定是否接听、返回码、Reason 和完整 SDP Answer
- 支持上层通过线程安全接口实时推送音频帧和视频帧
- 已预留单个媒体回调，可接收音频和视频 RTP 包
- `BYE` 后停止发送
- 仅支持单路会话，适合联调和后续扩展

未实现内容：

- 完整 SIP 事务/重传状态机
- Digest 鉴权
- 多路并发会话
- RTCP
- SRTP
- ICE / DTLS / SRTP
- RTP 解包后的 AAC/H264 帧回调或解码后 PCM/YUV 回调

## 媒体准备

先把 `dlc_video.mp4` 转成 `sipserver` 测试使用的裸码流：

```bash
chmod +x scripts/prepare_media.sh
./scripts/prepare_media.sh
```

默认会生成：

- `test_media/video.h264`
- `test_media/audio.aac`
- `test_media/audio.g711a`

说明：

- `video.h264` 会插入 AUD，便于最小 RTP 发送器按帧节奏发送
- `audio.aac` 为 ADTS 格式
- `audio.g711a` 为 8kHz/mono 原始 A-law 负载
- 如果源文件音轨本身是静音，脚本会自动为 `audio.g711a` 生成 440Hz 测试音，便于联调

## 编译

```bash
cmake -S . -B build
cmake --build build
```

生成可执行文件：

```bash
build/sipserver
```

## 运行

如果 SIP 客户端和本机不在同一台机器，`--media-ip` 必须设置为客户端可达的本机 IP。
和 Linphone 联调时，当前更建议使用 `--audio-codec g711a`。

AAC 模式：

```bash
./build/sipserver \
  --bind-ip 0.0.0.0 \
  --media-ip 192.168.1.10 \
  --audio-codec aac
```

G711A 模式：

```bash
./build/sipserver \
  --bind-ip 0.0.0.0 \
  --media-ip 192.168.1.10 \
  --audio-codec g711a
```

Linphone 示例：

```bash
./build/sipserver \
  --bind-ip 0.0.0.0 \
  --media-ip 192.168.18.126 \
  --sip-port 6060 \
  --audio-codec g711a
```

可使用如下目标地址发起呼叫：

```text
sip:test@192.168.18.126:6060;transport=udp
```

常用参数：

- `--sip-port`：SIP 监听端口，默认 `5060`
- `--audio-port`：本地音频 RTP 端口，默认 `5004`
- `--video-port`：本地视频 RTP 端口，默认 `5006`
- `--video-fps`：视频发送节奏，默认 `30`
- `--video-file`：H264 裸流文件
- `--aac-file`：AAC ADTS 文件
- `--g711a-file`：G711A 原始文件

## 联调说明

用标准 SIP 客户端发起 `INVITE` 时，Offer SDP 至少需要带上：

- `c=IN IP4 <client_ip>`
- `m=audio <rtp_port> RTP/AVP ...`
- `m=video <rtp_port> RTP/AVP ...`

当前实现对 SDP 的处理规则：

- 支持 `RTP/AVP` 和 `RTP/AVPF`
- 音频当前仅协商 `PCMA`
- 视频当前协商 `H264`
- 未接受的媒体行会在 Answer 中返回 `port 0`

服务端会根据 Offer 中的 IP/端口发送 RTP；在本机或 WSL 联调场景下，也会同时尝试向 `ACK` 的源地址发送 RTP，以规避客户端宣告地址和实际可达地址不一致的问题。

## 上层接管模式

当前仓库里的 `main` 已经整理成“上层 SDK 接入示例”。
它不是简单地直接调用 `sip_server_run()`，而是演示了上层怎样做这几件事：

- 保存当前会话对应的 `streamer_t *`
- 在 `on_invite` 里根据 Offer 决定是否接听，并生成 SDP Answer
- 在 `on_signal` 里感知 `INVITE`、`ACK`、`BYE`、会话建立、会话结束等关键 SIP 信令
- 在 `on_media` 里接收对端 RTP 包
- 在需要时通过线程安全接口实时喂入音频和视频帧

如果你只想保留原来的最小行为，也仍然可以直接调用：

```c
return sip_server_run(&config, &g_stop);
```

当前 `main` 有两种运行方式：

- 默认模式：
  上层仍然接管 SIP 信令和 SDP Answer，但媒体发送继续走 `streamer` 内部测试素材，优先保证 Linphone 联调稳定
- 推流演示模式：
  设置环境变量 `SIPSERVER_UPPER_PUSH_DEMO=1` 后，`main.c` 会额外启动两个示例线程，模拟上层持续调用 `streamer_push_audio_frame()` 和 `streamer_push_video_frame()` 喂流
  该模式下会把 `response->media.live_input_only` 置为 `1`，媒体只来自上层输入，不再回退到内部素材发送

入口仍然是：

```c
int sip_server_run_with_handlers(const app_config_t *config,
                                 volatile sig_atomic_t *stop_flag,
                                 const sip_server_handlers_t *handlers);
```

`main.c` 里可以直接参考的关键实现：

- `sample_sdk_t`
  示例 SDK 上下文，里面保存 `streamer_t *`、会话代数、配置和 demo 媒体状态
- `sample_sdk_on_signal()`
  接收关键 SIP 事件，并在 `SIP_SIGNAL_DIALOG_ESTABLISHED` / `SIP_SIGNAL_DIALOG_TERMINATED` 时更新当前会话的 `streamer`
- `sample_sdk_on_invite()`
  打印解析后的 Offer，并调用 `sample_sdk_build_answer()` 生成接听结果
- `sample_sdk_build_answer()`
  根据 Offer 中的 audio/video 情况填写 `sip_invite_response_t`
- `sample_sdk_push_audio_frame()` / `sample_sdk_push_video_frame()`
  演示上层如何把实时帧封装成 `streamer_audio_frame_t` / `streamer_video_frame_t` 后送入发送队列
- `sample_demo_audio_thread_main()` / `sample_demo_video_thread_main()`
  仅作为“上层实时喂流”的演示线程，实际项目里替换成你的采集、编码或转发线程即可

核心处理链路可以概括为：

```c
sip_server_handlers_t handlers;
sample_sdk_t sdk;

sample_sdk_init(&sdk, &config);
memset(&handlers, 0, sizeof(handlers));
handlers.on_signal = sample_sdk_on_signal;
handlers.on_invite = sample_sdk_on_invite;
handlers.on_media = sample_sdk_on_media;
handlers.user_data = &sdk;

return sip_server_run_with_handlers(&config, &g_stop, &handlers);
```

如果你自己接入上层实时音视频线程，送帧接口就是：

```c
streamer_t *current_streamer = /* 从 on_signal 或 SDK 上下文里取到当前会话 streamer */;

streamer_audio_frame_t audio_frame = {
    .data = g711_or_aac_payload,
    .size = payload_size,
    .timestamp = rtp_timestamp,
};

streamer_video_frame_t video_frame = {
    .data = h264_annexb_access_unit,
    .size = access_unit_size,
    .timestamp = rtp_timestamp,
};

streamer_push_audio_frame(current_streamer, &audio_frame);
streamer_push_video_frame(current_streamer, &video_frame);
```

说明：

- `streamer_push_audio_frame()` 和 `streamer_push_video_frame()` 是线程安全的队列投递接口
- 音频时间戳由上层按编码格式维护：
  `PCMA` 通常每 20ms 增加 `160`，`AAC-LC 48kHz` 每帧增加 `1024`
- 视频时间戳通常按 90k 时钟递增
- 视频输入应是完整 H264 Access Unit，内部会再做 RTP 分片
- `main.c` 里的 demo 线程正是按这个方式推送 `test_media/audio.g711a` 和 `test_media/video.h264`

`on_media` 回调参数定义见 `include/streamer.h`：

- `kind`：音频或视频
- `payload` / `payload_size`：RTP payload 原始负载
- `payload_type`、`sequence`、`timestamp`、`ssrc`：RTP 头关键信息
- `source_ip` / `source_port`：发送方地址

注意：

- 当前回调拿到的是原始 RTP payload，不是解码后的 PCM/YUV
- 发送线程和接收回调复用本地 RTP 端口
- `sip_server_run()` 仍然保留默认 SDP 协商逻辑；只有 `sip_server_run_with_handlers()` 才是上层完全接管模式
- 当前 `main.c` 示例默认按 `G711A + H264` 思路接管协商；如果你切到 `--audio-codec aac`，还需要在你的上层里补 AAC 的 Offer 解析和接听策略
- 如果你要接自己的 SDK，最直接的做法就是保留 `sample_sdk_on_signal()` / `sample_sdk_on_invite()` 这类回调骨架，把 demo 线程替换成你自己的采集或转发线程

## 后续扩展建议

- 增加 RTP 时间戳与帧边界的更精确建模
- 为 H264/AAC 增加更完整的 SDP 协商与参数校验
- 补充 RTCP、鉴权和注册表
- 拆分 SIP 会话层、媒体层、配置层，做成可嵌入库
