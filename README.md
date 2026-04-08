# Minimal SIP Server

一个最小可编译的 SIP Server/RTP 发送框架，使用 C 开发、CMake 构建，不依赖第三方运行库。工程目标是：

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

当前实现是“最小框架”，重点在工程骨架和媒体发送链路：

- 支持 SIP UDP 基础方法：`OPTIONS`、`REGISTER`、`INVITE`、`ACK`、`BYE`
- 收到 `INVITE` 后解析对端 SDP 中的音视频 RTP 端口
- 返回一个最小 SDP Answer，声明本端为 `sendonly`
- 收到 `ACK` 后启动音视频 RTP 发送线程
- `BYE` 后停止发送
- 仅支持单路会话，适合联调和后续扩展

未实现内容：

- 完整 SIP 事务/重传状态机
- Digest 鉴权
- 多路并发会话
- RTCP
- SRTP
- 复杂 SDP 能力协商

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

常用参数：

- `--sip-port`：SIP 监听端口，默认 `5060`
- `--audio-port`：本地音频 RTP 端口，默认 `5004`
- `--video-port`：本地视频 RTP 端口，默认 `5006`
- `--video-fps`：视频发送节奏，默认 `30`
- `--video-file`：H264 裸流文件
- `--aac-file`：AAC ADTS 文件
- `--g711a-file`：G711A 原始文件

## 联调说明

用标准 SIP 客户端发起 `INVITE` 时，需要在 Offer SDP 中带上：

- `c=IN IP4 <client_ip>`
- `m=audio <rtp_port> RTP/AVP ...`
- `m=video <rtp_port> RTP/AVP ...`

服务端会根据 Offer 中的 IP/端口，把 RTP 直接发回客户端。

## 后续扩展建议

- 增加 RTP 时间戳与帧边界的更精确建模
- 为 H264/AAC 增加更完整的 SDP 协商与参数校验
- 补充 RTCP、鉴权和注册表
- 拆分 SIP 会话层、媒体层、配置层，做成可嵌入库
