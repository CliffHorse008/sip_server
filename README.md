# Minimal SIP Server

一个最小可编译的 SIP Server/RTP 发送/接收框架，使用 C 开发、CMake 构建，不依赖第三方运行库。当前仓库只保留“上层推流演示”这一种运行方式。工程目标是：

- SIP 信令走 UDP，默认监听 `5060`
- 媒体传输走 UDP/RTP
- 视频支持 H264
- 音频支持 AAC 和 G711A
- 测试素材来自当前目录的 `dlc_video.mp4`
- `ffmpeg` 仅用于离线生成演示用测试码流，和 `sipserver` 运行时完全解耦

## 目录结构

```text
.
├── CMakeLists.txt
├── README.md
├── include/
│   ├── config.h
│   ├── sip_embed.h
│   ├── sip_server.h
│   └── streamer.h
├── scripts/
│   └── prepare_media.sh
├── src/
│   ├── config.c
│   ├── main.c
│   ├── sip_embed.c
│   ├── sip_server.c
│   └── streamer.c
└── test_media/
```

当前代码已经按职责拆成几层：

- 配置层：`config.h` / `config.c`
- 媒体层：`streamer.h` / `streamer.c`
- SIP 会话层：`sip_server.h` / `sip_server.c`
- 嵌入式接入层：`sip_embed.h` / `sip_embed.c`
- 示例宿主：`main.c`

## 功能边界

当前实现是“最小框架”，重点在工程骨架、上层送帧链路和 RTP 回调接入点：

- 支持 SIP UDP 基础方法：`OPTIONS`、`REGISTER`、`INVITE`、`ACK`、`BYE`
- 收到 `INVITE` 后解析对端 SDP 中的 `m=`、`rtpmap`、`fmtp`、方向属性和 RTP 端口
- 会按 Offer 生成最小可用的 SDP Answer，并沿用对端的 payload type
- 收到 `ACK` 后启动音视频 RTP 发送线程，等待上层持续送帧
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

同时会生成这些静态库目标：

- `libsipserver_config.a`
- `libsipserver_media.a`
- `libsipserver_session.a`
- `libsipserver_embed.a`

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

## 嵌入式库分层

当前仓库已经不是单纯的示例主程序，而是拆成可复用的静态库：

- `sipserver_config`
  负责默认值、参数解析、参数校验和配置结构体定义
- `sipserver_media`
  负责 RTP socket、发送队列、RTP 打包、媒体发送和接收回调
- `sipserver_session`
  负责 SIP 报文解析、事务/对话维护、Offer/Answer 驱动和媒体会话启动
- `sipserver_embed`
  负责给宿主提供一个更适合嵌入式接入的统一入口，内部维护当前活动会话、默认应答策略以及线程安全送帧接口

`main.c` 现在只保留为宿主示例，负责加载本地 demo 媒体并通过 `sip_embed_service_*()` 接口持续送帧。

## 上层推流演示

当前仓库里的 `main` 是嵌入式接入示例，也是现在唯一保留的运行方式。
它演示了宿主怎样做这几件事：

- 创建并运行 `sip_embed_service_t`
- 让嵌入式库内部处理 `INVITE`、`ACK`、`BYE`、会话建立和会话终止
- 在宿主线程里轮询当前会话状态并感知会话切换
- 在需要时通过线程安全接口实时喂入音频和视频帧

当前 `main` 的运行方式已经固定为上层推流演示：

当前行为：

- `main.c` 启动后会固定进入上层推流演示流程
- 会启动两个示例线程，模拟上层持续调用 `sip_embed_service_push_audio_frame()` 和 `sip_embed_service_push_video_frame()` 喂流
- `response->media.live_input_only` 固定置为 `1`
- `streamer` 内部不再加载或回退到默认测试素材发送；媒体完全来自上层输入队列

示例宿主的核心入口已经变成 `sip_embed_service`：

```c
sip_embed_service_t *sip_embed_service_create(const app_config_t *config);
int sip_embed_service_run(sip_embed_service_t *service,
                          volatile sig_atomic_t *stop_flag);
int sip_embed_service_push_audio_frame(sip_embed_service_t *service,
                                       const uint8_t *payload,
                                       size_t payload_size,
                                       uint32_t rtp_timestamp);
int sip_embed_service_push_video_frame(sip_embed_service_t *service,
                                       const uint8_t *access_unit,
                                       size_t access_unit_size,
                                       uint32_t rtp_timestamp);
```

`main.c` 里可以直接参考的关键实现：

- `sip_embed_service_create()` / `sip_embed_service_destroy()`
  创建和释放嵌入式服务实例
- `sip_embed_service_run()`
  以默认 SIP/媒体处理逻辑运行服务
- `sip_embed_service_get_stream_state()`
  读取当前是否存在活动会话以及会话代数，便于宿主在线程里感知切换
- `sip_embed_service_push_audio_frame()` / `sip_embed_service_push_video_frame()`
  宿主向当前活动会话投递实时音视频帧
- `sample_host_t`
  示例宿主上下文，内部保存 `sip_embed_service_t *` 与本地 demo 媒体缓存
- `sample_demo_audio_thread_main()` / `sample_demo_video_thread_main()`
  仅作为“上层实时喂流”的演示线程，实际项目里替换成你的采集、编码或转发线程即可

核心处理链路可以概括为：

```c
sip_embed_service_t *service = sip_embed_service_create(&config);
sample_host_init(&host, &config, service);

return sip_embed_service_run(service, &g_stop);
```

如果你自己接入上层实时音视频线程，送帧接口就是：

```c
sip_embed_service_t *service = sip_embed_service_create(&config);

sip_embed_service_push_audio_frame(service,
                                   g711_or_aac_payload,
                                   payload_size,
                                   rtp_timestamp);
sip_embed_service_push_video_frame(service,
                                   h264_annexb_access_unit,
                                   access_unit_size,
                                   rtp_timestamp);
```

说明：

- `sip_embed_service_push_audio_frame()` 和 `sip_embed_service_push_video_frame()` 底层仍然走线程安全队列投递
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
- `sip_server_run()` 和 `sip_server_run_with_handlers()` 仍然保留；但当前仓库更推荐通过 `sip_embed_service_run()` 作为嵌入式入口
- 当前 `main.c` 示例默认按 `G711A + H264` 思路接管协商；如果你切到 `--audio-codec aac`，还需要在你的上层里补 AAC 的 Offer 解析和接听策略
- 如果你要接自己的宿主程序，最直接的做法就是复用 `sip_embed_service_*()` 这一层，把 `main.c` 里的 demo 线程替换成你自己的采集、编码或转发线程

## 后续扩展建议

- 增加 RTP 时间戳与帧边界的更精确建模：
  当前示例主要依赖上层直接给出时间戳，后续可以把“采样时钟域”和“发送时钟域”明确拆开，分别维护音频帧持续时长、视频帧显示时间、RTP 时钟换算和发送抖动补偿。
- 对音频建立更精确的时间戳模型：
  对 `G711A` 明确按采样数累计时间戳，对 `AAC` 明确按每帧 sample 数、采样率和打包粒度推进时间戳；如果后续支持多帧聚合、静音补偿或重采样，也应在这一层统一建模。
- 对视频建立更精确的帧边界模型：
  当前输入假设是完整 H264 Access Unit，后续可以继续细化为“帧边界识别层 + RTP 分片层”，明确 IDR、非 IDR、AUD、SPS、PPS 的处理策略，并把帧时间戳、marker bit、重传缓存边界统一到 Access Unit 级别。
- 为收包侧补齐帧重组模型：
  当前 `on_media` 回调拿到的是原始 RTP payload，后续如果要做嵌入式 SDK，更适合增加 RTP 重排序、丢包检测、H264 FU-A 重组、AAC 聚合解析，再向上层输出完整帧事件而不是裸 payload。
- 为 H264/AAC 增加更完整的 SDP 协商与参数校验
- 补充 RTCP、鉴权和注册表
- SIP 会话层建议职责单独收口：
  负责 SIP 报文解析、事务状态机、对话生命周期、上层回调派发，以及与媒体层之间的会话参数衔接；对外暴露“收到 INVITE/ACK/BYE 后怎么驱动状态机”的清晰 API。
- 媒体层建议拆成发送、接收、协商三个子模块：
  发送模块只关心队列、打包和 socket 发送；接收模块只关心 RTP 接收、重排和帧重组；协商模块只关心 SDP 生成与解析，这样后续替换编码器或网络栈时影响面更小。
- 配置层建议从命令行解析中剥离：
  保留纯结构体初始化、默认值、参数校验和运行期只读配置对象，让库既能被命令行程序调用，也能被 GUI、守护进程或其他 SDK 宿主直接嵌入。
- 对外接口建议按“宿主可控”思路设计：
  初始化、启动会话、停止会话、推帧、收帧回调、错误上报、日志钩子都应独立成稳定 API，避免未来库化之后仍然强依赖当前 demo 线程和全局变量。
