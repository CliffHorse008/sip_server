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
- 会按 Offer 生成最小可用的 SDP Answer，并沿用对端的 payload type
- 收到 `ACK` 后启动音视频 RTP 发送线程
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

## 媒体回调

当前默认的 `main` 只是启动 SIP 服务，不会自动注册媒体回调。
如果你需要在程序里统一处理收到的音频和视频 RTP，可以使用 `sip_server_run_with_callback()`：

```c
#include "sip_server.h"

static void on_media(const streamer_rtp_packet_t *packet, void *user_data)
{
    (void) user_data;

    if (packet->kind == STREAMER_MEDIA_AUDIO) {
        /* packet->payload / packet->payload_size */
    } else {
        /* video */
    }
}
```

```c
return sip_server_run_with_callback(&config, &g_stop, on_media, NULL);
```

回调参数定义见 `include/streamer.h`：

- `kind`：音频或视频
- `payload` / `payload_size`：RTP payload 原始负载
- `payload_type`、`sequence`、`timestamp`、`ssrc`：RTP 头关键信息
- `source_ip` / `source_port`：发送方地址

注意：

- 当前回调拿到的是原始 RTP payload，不是解码后的 PCM/YUV
- 发送线程和接收回调复用本地 RTP 端口

## 后续扩展建议

- 增加 RTP 时间戳与帧边界的更精确建模
- 为 H264/AAC 增加更完整的 SDP 协商与参数校验
- 补充 RTCP、鉴权和注册表
- 拆分 SIP 会话层、媒体层、配置层，做成可嵌入库
