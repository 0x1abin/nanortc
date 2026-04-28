# NanoRTC — 架构设计文档

> 一个面向 RTOS/嵌入式系统的 Sans I/O、可裁剪 WebRTC 纯 C 实现。
>
> 本文档作为 NanoRTC 的权威设计参考，用于指导 AI 辅助开发。

---

## 1. 项目概述

### 1.1 NanoRTC 是什么

NanoRTC 是一个**纯 C**、**Sans I/O** 架构的 WebRTC 实现，面向运行 FreeRTOS、Zephyr、
RT-Thread 等 RTOS 的资源受限微控制器。从第一行代码开始就为嵌入式设备设计。

NanoRTC 支持**编译时特性裁剪**，开发者可以根据产品需求只包含必要的功能——从最小的
DataChannel-only 构建（~60KB RAM）到完整的音视频媒体传输。

### 1.2 设计原则

| # | 原则 | 理由 |
|---|------|------|
| 1 | **Sans I/O** | NanoRTC 不持有 socket，不创建线程，不调用任何操作系统 API。它是一个纯粹的状态机，完全由外部输入驱动。这使其天然具备可移植性和可测试性。 |
| 2 | **可插拔加密** | 默认加密库是 **mbedtls**（嵌入式平台内置），也支持 **OpenSSL**（Linux 主机开发）。加密操作通过可插拔的 provider 接口抽象，编译时选择后端。 |
| 3 | **编译时可裁剪** | 正交特性标志（`NANORTC_FEATURE_DATACHANNEL`、`NANORTC_FEATURE_AUDIO`、`NANORTC_FEATURE_VIDEO`）允许任意组合，代码体积和 RAM 占用按功能缩放。 |
| 4 | **以 lwIP BSD Socket 为网络基线** | NanoRTC 自身从不调用 socket API。应用层的事件循环使用 lwIP BSD socket API（≥ 2.1.0）进行网络 I/O，该接口在几乎所有嵌入式平台上都可用。 |
| 5 | **RTOS 无关** | NanoRTC 核心内部不包含任何 FreeRTOS 特定 API。平台差异（线程、定时器、熵源）完全在应用层事件循环中处理，不在库内部。 |

### 1.3 特性标志

```c
// nanortc_config.h — 正交特性标志，任意组合

#define NANORTC_FEATURE_DATACHANNEL  1   // SCTP + DCEP DataChannel
#define NANORTC_FEATURE_DC_RELIABLE  1   // 可靠重传（DC 子特性）
#define NANORTC_FEATURE_DC_ORDERED   1   // 有序交付（DC 子特性）
#define NANORTC_FEATURE_AUDIO        0   // 音频 (RTP/SRTP/Jitter)
#define NANORTC_FEATURE_VIDEO        0   // 视频 (RTP/SRTP/BWE)
#define NANORTC_FEATURE_H265         0   // H.265/HEVC（VIDEO 子特性）
#define NANORTC_FEATURE_IPV6         1   // IPv6 candidate 解析/生成
#define NANORTC_FEATURE_TURN         1   // TURN relay client
```

特性标志 → 模块映射：

| 特性标志 | 编译的模块 |
|----------|-----------|
| *(核心，始终包含)* | rtc, ice, stun, dtls, sdp, crc32 |
| `NANORTC_FEATURE_DATACHANNEL` | sctp, datachannel, crc32c |
| `NANORTC_FEATURE_AUDIO` 或 `VIDEO` | rtp, rtcp, srtp |
| `NANORTC_FEATURE_AUDIO` | jitter |
| `NANORTC_FEATURE_VIDEO` | media, rtcp feedback, h264, annex_b, bwe, twcc |
| `NANORTC_FEATURE_H265` | h265, base64（依赖 `VIDEO`） |
| `NANORTC_FEATURE_TURN` | turn |
| `NANORTC_FEATURE_IPV6` | IPv6 address parsing/formatting in addr |

CI 测试的 7 种 canonical 组合：

| 名称 | DC | AUDIO | VIDEO | H265 |
|------|-----|-------|-------|------|
| DATA | ON | OFF | OFF | OFF |
| AUDIO | ON | ON | OFF | OFF |
| MEDIA | ON | ON | ON | OFF |
| MEDIA_H265 | ON | ON | ON | ON |
| AUDIO_ONLY | OFF | ON | OFF | OFF |
| MEDIA_ONLY | OFF | ON | ON | OFF |
| CORE_ONLY | OFF | OFF | OFF | OFF |

当前资源占用以 `docs/engineering/memory-profiles.md` 为准。ESP32-P4 / ESP-IDF 5.5 / `-Os` Kconfig 默认值的代表性数据如下：

| 组合 | Flash (.text) | `sizeof(nanortc_t)` |
|------|---------------|----------------------|
| CORE_ONLY | 29.0 KB | 10.2 KB |
| DC only | 38.8 KB | 19.4 KB |
| Audio only | 40.8 KB | 20.6 KB |
| DC + Audio | 50.6 KB | 29.9 KB |
| Media only | 45.3 KB | 51.0 KB |
| DC + Audio + Video | 55.0 KB | 60.3 KB |

---

## 2. 架构设计

### 2.1 Sans I/O 模型

NanoRTC 遵循 [Sans I/O](https://sans-io.readthedocs.io) 模式。核心
`nanortc_t` 是一个无副作用的纯状态机；协议细节以 RFC 为唯一权威来源，
第三方实现只可作为 API 形态或测试夹具的工程背景，不作为 wire format /
状态机行为依据：

```
                    ┌─────────────────────────────────────┐
   用户输入:         │                                     │  输出:
                    │                                     │
   UDP 数据 ------->│         nanortc_t                  │-------> 待发送的 UDP 数据
   时间戳 --------->│       (纯状态机)                     │-------> 事件（DC 数据、媒体）
   用户指令 ------->│                                     │-------> 下次超时时间
                    │  无 socket. 无线程. 无 malloc.       │
                    └─────────────────────────────────────┘
```

**关键特性：**

- NanoRTC **绝不**调用 `socket()`, `sendto()`, `select()` 或任何网络 API
- NanoRTC **绝不**调用 `malloc()` 或 `free()`（使用调用者提供的 buffer 或静态池）
- NanoRTC **绝不**调用 `pthread_create()` 或任何线程 API
- NanoRTC **绝不**调用 `clock_gettime()` 或任何时钟 API——时间由外部传入
- NanoRTC **可以超实时速度测试**——通过注入合成时间戳驱动
- NanoRTC **没有内部互斥锁**——线程安全由调用者负责

### 2.2 公共 API

```c
// ---- 生命周期 ----
int  nanortc_init(nanortc_t *rtc, const nanortc_config_t *cfg);
void nanortc_destroy(nanortc_t *rtc);

// ---- SDP ----
int  nanortc_accept_offer(nanortc_t *rtc, const char *offer,
                       char *answer_buf, size_t answer_buf_len,
                       size_t *out_len);
int  nanortc_create_offer(nanortc_t *rtc,
                       char *offer_buf, size_t offer_buf_len,
                       size_t *out_len);
int  nanortc_accept_answer(nanortc_t *rtc, const char *answer);

// ---- ICE ----
int  nanortc_add_local_candidate(nanortc_t *rtc,
                              const char *ip, uint16_t port);
int  nanortc_add_remote_candidate(nanortc_t *rtc,
                               const char *candidate_str);

// ---- 事件循环（Sans I/O 核心）----
int  nanortc_poll_output(nanortc_t *rtc, nanortc_output_t *out);
int  nanortc_handle_input(nanortc_t *rtc, const nanortc_input_t *in);
//   nanortc_input_t { now_ms, data, len, src, dst } — 与 nanortc_output_t.transmit 对称

// ---- DataChannel (flat API — no handle needed) ----
#if NANORTC_FEATURE_DATACHANNEL
int  nanortc_create_datachannel(nanortc_t *rtc, const char *label,
                               const nanortc_datachannel_options_t *options);
int  nanortc_datachannel_send(nanortc_t *rtc, uint16_t id, const void *data, size_t len);
int  nanortc_datachannel_send_string(nanortc_t *rtc, uint16_t id, const char *str);
int  nanortc_datachannel_close(nanortc_t *rtc, uint16_t id);
const char *nanortc_datachannel_get_label(nanortc_t *rtc, uint16_t id);
#endif

// ---- 媒体 ----
#if NANORTC_HAVE_MEDIA_TRANSPORT
int  nanortc_add_audio_track(nanortc_t *rtc, nanortc_direction_t dir,
                             nanortc_codec_t codec, uint32_t sample_rate,
                             uint8_t channels);
int  nanortc_add_video_track(nanortc_t *rtc, nanortc_direction_t dir,
                             nanortc_codec_t codec);
void nanortc_set_direction(nanortc_t *rtc, uint8_t mid, nanortc_direction_t dir);
int  nanortc_send_audio(nanortc_t *rtc, uint8_t mid, uint32_t pts_ms,
                        const void *data, size_t len);
int  nanortc_send_video(nanortc_t *rtc, uint8_t mid, uint32_t pts_ms,
                        const void *data, size_t len);
int  nanortc_request_keyframe(nanortc_t *rtc, uint8_t mid);
#endif

// ---- 连接状态 ----
bool nanortc_is_alive(const nanortc_t *rtc);
bool nanortc_is_connected(const nanortc_t *rtc);
void nanortc_disconnect(nanortc_t *rtc);
```

### 2.3 输出/事件类型

```c
typedef enum {
    NANORTC_OUTPUT_TRANSMIT,   // 需要通过 UDP socket 发送的数据
    NANORTC_OUTPUT_EVENT,      // 应用层事件
    NANORTC_OUTPUT_TIMEOUT,    // 下次需要调用 handle_input 的时间
} nanortc_output_type_t;

typedef enum {
    // 连接生命周期
    NANORTC_EV_CONNECTED = 0,            // ICE+DTLS(+SCTP) 全部建立
    NANORTC_EV_DISCONNECTED = 1,         // 连接断开
    NANORTC_EV_ICE_STATE_CHANGE = 2,     // ICE 状态变化

    // 媒体（typed events）
    NANORTC_EV_MEDIA_ADDED = 3,          // 远端添加了新媒体轨道
    NANORTC_EV_MEDIA_CHANGED = 4,        // 媒体方向变化
    NANORTC_EV_MEDIA_DATA = 5,           // 收到媒体帧（音频或视频）
    NANORTC_EV_KEYFRAME_REQUEST = 6,     // 远端请求关键帧

    // DataChannel
    NANORTC_EV_DATACHANNEL_OPEN = 7,         // DataChannel 打开
    NANORTC_EV_DATACHANNEL_DATA = 8,         // DataChannel 数据（binary 标志区分二进制/字符串）
    NANORTC_EV_DATACHANNEL_CLOSE = 9,        // DataChannel 关闭
    NANORTC_EV_DATACHANNEL_BUFFERED_LOW = 10,// 发送缓冲区低于阈值
} nanortc_event_type_t;

// 事件结构体 (tagged union)
typedef struct nanortc_event {
    nanortc_event_type_t type;
    union {
        nanortc_ev_media_added_t     media_added;
        nanortc_ev_media_changed_t   media_changed;
        nanortc_ev_media_data_t      media_data;
        nanortc_ev_keyframe_request_t keyframe_request;
        nanortc_ev_datachannel_open_t    datachannel_open;
        nanortc_ev_datachannel_data_t    datachannel_data;
        nanortc_ev_datachannel_id_t      datachannel_id;
        uint16_t                     ice_state;
    };
} nanortc_event_t;
```

### 2.4 内部模块结构

```
nanortc/
├── include/
│   └── nanortc.h                  // 公共 API（单一头文件）
│
├── src/
│   ├── nano_rtc.c                 // 主状态机, poll/handle
│   ├── nano_sdp.c                 // SDP 解析/生成
│   ├── nano_ice.c                 // ICE: controlled + controlling 角色
│   ├── nano_stun.c                // STUN 消息编解码
│   ├── nano_dtls.c                // DTLS 状态机（通过 crypto provider）
│   ├── nano_sctp.c                // SCTP-Lite 状态机
│   ├── nano_datachannel.c         // DCEP 协议 + DataChannel 逻辑
│   ├── nano_crc32c.c              // CRC-32c（SCTP 校验和，查表实现）
│   │
│   │   // 条件编译: NANORTC_FEATURE_AUDIO || NANORTC_FEATURE_VIDEO
│   ├── nano_rtp.c                 // RTP 打包/解包
│   ├── nano_rtcp.c                // RTCP SR/RR/NACK/PLI
│   ├── nano_srtp.c                // SRTP 加解密
│   ├── nano_jitter.c              // Jitter buffer (NANORTC_FEATURE_AUDIO)
│   │
│   │   // 条件编译: NANORTC_FEATURE_VIDEO
│   └── nano_bwe.c                 // 带宽估计
│
├── crypto/
│   ├── nanortc_crypto.h              // 可插拔 crypto provider 接口
│   └── nanortc_crypto_mbedtls.c      // 默认实现: mbedtls
│
├── examples/
│   ├── esp32_datachannel/         // ESP-IDF + MQTT 信令
│   ├── esp32_audio_intercom/      // ESP-IDF + 音频收发
│   ├── esp32_camera/              // ESP-IDF + H.264 视频
│   ├── linux_echo/                // Linux 测试环境
│   └── common/
│       ├── signaling_mqtt.c       // 基于 MQTT 的 SDP 交换
│       ├── signaling_http_whip.c  // WHIP 协议信令
│       └── run_loop.c             // 可复用的 select() 事件循环
│
├── tests/
│   ├── test_stun.c                // STUN 编解码单元测试
│   ├── test_sdp.c                 // SDP 解析/生成测试
│   ├── test_sctp.c                // SCTP 状态机测试
│   ├── test_dtls.c                // DTLS 握手模拟
│   ├── test_e2e.c                 // 端到端测试（合成数据，无需网络）
│   └── test_main.c                // 测试入口
│
├── CMakeLists.txt
├── idf_component.yml              // ESP-IDF 组件清单
├── Kconfig                        // ESP-IDF menuconfig 集成
└── LICENSE                        // MIT
```

### 2.5 条件编译

```c
// nano_rtc.c 内部 — 基于特性标志包含模块

#include "nano_sdp.h"
#include "nano_ice.h"
#include "nano_dtls.h"

#if NANORTC_FEATURE_DATACHANNEL
#include "nano_sctp.h"
#include "nano_datachannel.h"
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
#include "nano_rtp.h"
#include "nano_rtcp.h"
#include "nano_srtp.h"
#endif

#if NANORTC_FEATURE_AUDIO
#include "nano_jitter.h"
#endif

#if NANORTC_FEATURE_VIDEO
#include "nano_bwe.h"
#endif
```

SDP 生成根据特性标志自适应：

```c
// nano_sdp.c
void nano_sdp_generate(nano_sdp_t *sdp, char *buf, size_t len) {
    // 始终包含: 会话级字段 (v=, o=, s=, t=, ice-ufrag, ice-pwd, fingerprint)

#if NANORTC_FEATURE_DATACHANNEL
    // DataChannel m-line
    sdp_append_datachannel_mline(sdp, buf, len);
#endif

#if NANORTC_FEATURE_AUDIO
    // 音频 m-line（含编解码器协商）
    sdp_append_audio_mline(sdp, buf, len);
#endif

#if NANORTC_FEATURE_VIDEO
    // 视频 m-line（含编解码器协商）
    sdp_append_video_mline(sdp, buf, len);
#endif
}
```

---

## 3. 协议栈 — 逐层说明

### 3.1 数据流

所有数据通过 `nanortc_handle_input()` 进入、通过 `nanortc_poll_output()` 输出，
均为字节缓冲区。内部根据 UDP 包首字节进行解复用（RFC 7983）：

```
收到的 UDP 包
       │
       ▼
  首字节判断 (RFC 7983 复用)
       │
       ├── [0x00-0x03]  → STUN  → nano_ice.c
       ├── [0x14-0x40]  → DTLS  → nano_dtls.c
       │                            │
       │                   ┌────────┴────────┐
       │                   ▼                 ▼
       │              SCTP chunk        SRTP 密钥相关
       │              nano_sctp.c       (NANORTC_FEATURE_DATACHANNEL)
       │                   │
       │                   ▼
       │           DataChannel 消息
       │           nano_datachannel.c
       │
       └── [0x80-0xBF]  → SRTP (NANORTC_HAVE_MEDIA_TRANSPORT)
                            → nano_srtp.c → nano_rtp.c
```

说明：SCTP 和 SRTP 共用同一个 DTLS 关联。SCTP 包作为 DTLS 应用数据传输。
RTP/SRTP 包绕过 DTLS，在 SRTP 层直接加密（密钥从 DTLS 的
`export_keying_material` 导出）。

### 3.2 ICE（nano_ice.c / nano_stun.c）

**范围**：支持两种 ICE 角色（RFC 8445）。

NanoRTC 的 ICE agent 支持两种模式：

**Controlled 角色（answerer，ICE-Lite 行为）：**

1. 响应收到的 STUN Binding Request，返回 Binding Response
2. 使用本地 ICE 密码验证 `MESSAGE-INTEGRITY`（HMAC-SHA1）
3. 追加 `FINGERPRINT`（CRC-32 XOR 0x5354554E）
4. 确认连通性后向上层报告选中的远端候选

**Controlling 角色（offerer，主动发起连通性检查）：**

1. 主动发送 STUN Binding Request 给远端候选
2. 处理 Binding Response，验证 `MESSAGE-INTEGRITY`
3. 驱动候选配对和优先级排序
4. 发送 `USE-CANDIDATE` 属性提名选中的候选对

设备既可以作为 answerer（被浏览器/SFU 连接），也可以作为 offerer（主动连接远端）。
通过 `nanortc_config_t.role` 选择角色：`NANORTC_ROLE_CONTROLLED` 或 `NANORTC_ROLE_CONTROLLING`。

**STUN 消息格式**（nano_stun.c，约 400 行）：

- STUN 消息编解码（RFC 8489）
- 支持属性：`MAPPED-ADDRESS`, `XOR-MAPPED-ADDRESS`, `USERNAME`,
  `MESSAGE-INTEGRITY`, `FINGERPRINT`, `ICE-CONTROLLED`, `PRIORITY`, `USE-CANDIDATE`
- HMAC-SHA1 通过 crypto provider 或自包含实现（约 150 行）
- CRC-32 查表实现（约 30 行）

### 3.3 DTLS 1.2（nano_dtls.c）

**范围**：DTLS 1.2 握手和记录层，委托给 crypto provider。

NanoRTC 不自行实现 DTLS。crypto provider（通常是 mbedtls）处理完整的 DTLS 状态机。
NanoRTC 提供 BIO（缓冲 I/O）接口：

```c
// nano_dtls.c 驱动 DTLS 握手，不触碰 socket

typedef struct {
    // 出站: 待发送的 DTLS 记录（由 mbedtls 填充，poll_output 消费）
    uint8_t  out_buf[NANORTC_DTLS_BUF_SIZE];
    size_t   out_len;

    // 入站: 收到的 DTLS 记录（由 handle_receive 填充）
    uint8_t  in_buf[NANORTC_DTLS_BUF_SIZE];
    size_t   in_len;

    // 状态
    nano_dtls_state_t state;  // INIT → HANDSHAKING → ESTABLISHED → CLOSED
    nanortc_crypto_dtls_ctx_t crypto_ctx;
} nano_dtls_t;
```

**自签名证书**：初始化时通过 crypto provider 生成。证书指纹嵌入 SDP answer
中用于 DTLS-SRTP 验证。

### 3.4 SCTP-Lite（nano_sctp.c）

**范围**：RFC 4960 的最小 SCTP 子集，用于 WebRTC DataChannel 传输。

这是最具挑战性的自研模块。NanoRTC 仅实现 WebRTC Data Channel 规范（RFC 8831）
所需的功能：

**已实现：**

- SCTP-over-DTLS 封装（RFC 8261）
- 单关联（无多宿主）
- 四次握手：INIT → INIT-ACK → COOKIE-ECHO → COOKIE-ACK
- DATA / SACK chunk 处理（选择性确认）
- 有序和无序交付
- 流管理（多 DataChannel 流）
- 分片与重组
- 重传定时器（基础 RTO 计算）
- FORWARD-TSN，用于不可靠 DataChannel（RFC 3758）
- HEARTBEAT / HEARTBEAT-ACK 保活
- SHUTDOWN 关闭序列

**不实现：**

- 多宿主（单端点多 IP）
- Path MTU 发现（使用固定 MTU）
- 动态地址重配置（RFC 5061）
- SCTP 认证（RFC 4895）— 在 DTLS 之上不需要

**状态机：**

```
CLOSED ──(收到 INIT)──> COOKIE_WAIT
    │                        │
    │                   (发送 INIT-ACK 含 cookie)
    │                        │
    │                   (收到 COOKIE-ECHO)
    │                        │
    │                        ▼
    │                   ESTABLISHED ──(收到 SHUTDOWN)──> SHUTDOWN_RECEIVED
    │                        │                                │
    │                   (收发 DATA)                      (发送 SHUTDOWN-ACK)
    │                        │                                │
    │                   (发送 SHUTDOWN)                  (收到 SHUTDOWN-COMPLETE)
    │                        │                                │
    │                        ▼                                ▼
    └────────────────── CLOSED <───────────────────────── CLOSED
```

**预估实现量**：约 2000-3000 行 C 代码。

### 3.5 DataChannel（nano_datachannel.c）

**范围**：WebRTC 数据通道建立协议（DCEP，RFC 8832）。

- DATA_CHANNEL_OPEN 消息解析/生成
- DATA_CHANNEL_ACK 响应
- 通道类型映射：可靠、不可靠（最大重传次数 / 最大生命周期）
- 有序 vs 无序交付（映射到 SCTP 流参数）
- 字符串 vs 二进制消息类型（PPID: 51 为字符串, 53 为二进制）
- 多个 DataChannel 使用不同 SCTP stream ID 并发

**预估实现量**：约 300-500 行 C 代码。

### 3.6 RTP/RTCP（nano_rtp.c, nano_rtcp.c）— NANORTC_HAVE_MEDIA_TRANSPORT

**RTP（nano_rtp.c）：**

- RTP 头编解码（RFC 3550）
- 从 SDP 协商映射 payload type
- 序列号和时间戳管理
- 音频打包：Opus, G.711（A-law / μ-law）
- 视频打包（NANORTC_FEATURE_VIDEO）：H.264（FU-A）, VP8

**RTCP（nano_rtcp.c）：**

- 发送方报告（SR）生成
- 接收方报告（RR）生成
- NACK（通用否定确认，RFC 4585）
- PLI（图片丢失指示）— 关键帧请求（NANORTC_FEATURE_VIDEO）
- REMB（接收方估计最大带宽）— NANORTC_FEATURE_VIDEO
- Transport-wide CC feedback 解析（RTCP PT=205 FMT=15，draft-holmer-rmcat-twcc-01）— NANORTC_FEATURE_VIDEO

### 3.6.1 带宽估计（nano_bwe.c / nano_twcc.c）— NANORTC_FEATURE_VIDEO

NanoRTC 只做**感知**，不做**执行**：BWE 消费 REMB 和 TWCC 反馈得出一个建议码率，应用层自行据此驱动编码器。库内不做 pacer、不做主动丢帧、不做 GCC delay-based 控制。

**输入：**
- RTCP PSFB FMT=15 REMB — 直接给出目标码率，EMA 平滑。
- RTCP RTPFB FMT=15 TWCC — 从 chunk + delta 序列重建 `received_count / packet_status_count`，套 loss-based 控制器：
  - loss < ~2 %：估计 × 1.08
  - 2 %–10 %：保持
  - \> 10 %：估计 × (512 − loss_q8) / 512
  
  再与现有估计做 EMA 融合。

**发送侧协商：** 视频 m-line 在 SDP 里发布 `a=extmap:<id> http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01`（默认 ID 3，可回显对端）。RTP 发送时在 one-byte header extension（`0xBEDE`）里写入 transport-cc 16-bit 序列号，每包开销 8 字节。

**输出：**
- `NANORTC_EV_BITRATE_ESTIMATE` 事件：含 `bitrate_bps / prev_bitrate_bps / direction (UP/DOWN/STABLE) / source (REMB/TWCC_LOSS)`。
- `nanortc_get_estimated_bitrate(rtc)` 查询。
- `nanortc_track_stats_t`：`estimated_bitrate_bps`、`send_bitrate_bps`（1 秒滑窗）、`send_fps_q8`（Q8.8）、`fraction_lost`（从 RR/SR report block 提取）。

**运行时调参：** `nanortc_set_bitrate_bounds(rtc, min, max)` / `nanortc_set_initial_bitrate(rtc, bps)` / `nanortc_set_bwe_event_threshold(rtc, pct)` 覆盖编译期 `NANORTC_BWE_*` 宏。适用于 IoT 设备启动后才知道硬件编码器能力的场景。

**不做：** 发送 TWCC feedback 包（接收端需要，Phase 10 候选）、GCC/trendline 延迟控制、pacer、独立 NACK/PLI/FIR 事件（当前 `NANORTC_EV_KEYFRAME_REQUEST` 足够）。

### 3.7 SRTP（nano_srtp.c）— NANORTC_HAVE_MEDIA_TRANSPORT

- AES-128-CM + HMAC-SHA1-80（RFC 8827 强制要求）
- 从 DTLS `export_keying_material()` 导出密钥
- 滑动窗口重放保护
- 通过 crypto provider 调用 AES 和 HMAC 原语

### 3.8 Jitter Buffer（nano_jitter.c）— NANORTC_FEATURE_AUDIO

- 固定大小环形缓冲区（可配置深度）
- 基于序列号重排序
- 可配置播放延迟
- 音频：帧级输出
- 视频：从 RTP FU-A 包重组 NAL 单元

### 3.9 SDP（nano_sdp.c）

根据特性标志自适应的 SDP 生成和解析：

- `NANORTC_FEATURE_DATACHANNEL`：生成 `m=application` 行（SCTP/DTLS）
- `NANORTC_FEATURE_AUDIO`：增加 `m=audio` 行及协商的编解码器
- `NANORTC_FEATURE_VIDEO`：增加 `m=video` 行

解析提取：ICE 凭据（ufrag/pwd）、DTLS 指纹、候选地址、编解码器参数、m-line 方向性。

**预估实现量**：约 500-800 行 C 代码。

---

## 4. Crypto Provider 接口

### 4.1 设计

NanoRTC 将所有加密操作抽象在可插拔的 provider 接口后面。用户可以将 mbedtls
替换为 wolfSSL、BearSSL、OpenSSL、硬件加密加速器或平台特定实现，无需修改 NanoRTC 核心代码。
Linux 主机开发可使用 OpenSSL 后端（`-DNANORTC_CRYPTO=openssl`）。

```c
// nanortc_crypto.h

typedef struct nanortc_crypto_provider {
    const char *name;  // 例如 "mbedtls", "wolfssl", "hw-aes"

    // ---- DTLS（必需）----
    int  (*dtls_init)(nanortc_crypto_dtls_ctx_t *ctx, int is_server);
    int  (*dtls_set_bio)(nanortc_crypto_dtls_ctx_t *ctx,
                         void *userdata,
                         nanortc_dtls_send_fn send_cb,
                         nanortc_dtls_recv_fn recv_cb);
    int  (*dtls_handshake)(nanortc_crypto_dtls_ctx_t *ctx);
    int  (*dtls_encrypt)(nanortc_crypto_dtls_ctx_t *ctx,
                         const uint8_t *in, size_t in_len,
                         uint8_t *out, size_t *out_len);
    int  (*dtls_decrypt)(nanortc_crypto_dtls_ctx_t *ctx,
                         const uint8_t *in, size_t in_len,
                         uint8_t *out, size_t *out_len);
    int  (*dtls_export_keying_material)(nanortc_crypto_dtls_ctx_t *ctx,
                                        const char *label,
                                        uint8_t *out, size_t out_len);
    int  (*dtls_get_fingerprint)(nanortc_crypto_dtls_ctx_t *ctx,
                                 char *buf, size_t buf_len);
    void (*dtls_free)(nanortc_crypto_dtls_ctx_t *ctx);

    // ---- HMAC-SHA1（必需，用于 STUN MESSAGE-INTEGRITY）----
    void (*hmac_sha1)(const uint8_t *key, size_t key_len,
                      const uint8_t *data, size_t data_len,
                      uint8_t out[20]);

    // ---- CSPRNG（必需）----
    int  (*random_bytes)(uint8_t *buf, size_t len);

    // ---- SRTP（AUDIO/MEDIA profile 必需）----
#if NANORTC_HAVE_MEDIA_TRANSPORT
    int  (*aes_128_cm)(const uint8_t key[16], const uint8_t iv[16],
                       const uint8_t *in, size_t len, uint8_t *out);
    void (*hmac_sha1_80)(const uint8_t *key, size_t key_len,
                         const uint8_t *data, size_t data_len,
                         uint8_t out[10]);
#endif

} nanortc_crypto_provider_t;

// 默认 mbedtls 实现（随 NanoRTC 一起发布）
const nanortc_crypto_provider_t *nanortc_crypto_mbedtls(void);
```

### 4.2 配置示例

```c
nanortc_config_t cfg = {
    .crypto = nanortc_crypto_mbedtls(),
    .role = NANORTC_ROLE_CONTROLLED,       // ICE controlled (answerer)

    // 内存配置
    .sctp_send_buf_size = 64 * 1024,    // 64KB 发送缓冲区
    .sctp_recv_buf_size = 64 * 1024,    // 64KB 接收缓冲区

#if NANORTC_HAVE_MEDIA_TRANSPORT
    .audio_codec = NANORTC_CODEC_OPUS,
    .audio_sample_rate = 48000,
    .audio_channels = 1,
    .audio_direction = NANORTC_DIR_SENDRECV,
    .jitter_depth_ms = 100,
#endif

#if NANORTC_FEATURE_VIDEO
    .video_codec = NANORTC_CODEC_H264,
    .video_direction = NANORTC_DIR_SENDONLY,
#endif
};
```

---

## 5. 平台集成

### 5.1 NanoRTC 不做的事（应用层职责）

| 职责 | 应用层如何处理 |
|------|--------------|
| 创建/绑定 UDP socket | 通过 lwIP BSD socket API 调用 `socket()` + `bind()` |
| 发送 UDP 包 | 使用 `NANORTC_OUTPUT_TRANSMIT` 中的数据调用 `sendto()` |
| 接收 UDP 包 | 调用 `recvfrom()` 然后传给 `nanortc_handle_input()` |
| 事件循环 / task | 单个 FreeRTOS task 中运行 `select()` 循环 |
| 时间源 | 向 handle 函数传入单调毫秒值 |
| 信令（SDP 交换） | MQTT / WebSocket / HTTP — 用户自选 |
| 网卡枚举 | 读取 lwIP `netif_list` 或硬编码 IP |
| STUN/TURN 服务器发现 | 用户配置服务器地址 |
| 信令层 TLS | 与 NanoRTC 独立（如 `esp-tls`） |
| 音视频采集与编码 | 应用层编解码器 + 硬件 |

### 5.2 lwIP 要求

```c
// 最低 lwIP 版本和配置要求

#include <lwip/init.h>
#if LWIP_VERSION_MAJOR < 2
  #error "nanortc requires lwIP >= 2.0.0"
#endif
#if LWIP_VERSION_MAJOR == 2 && LWIP_VERSION_MINOR < 1
  #warning "lwIP 2.0.x: limited support. Recommend >= 2.1.0"
#endif
```

必需的 lwIP 选项（`lwipopts.h`）：

| 选项 | 值 | 说明 |
|------|---|------|
| `LWIP_SOCKET` | 1 | BSD socket API（必需） |
| `LWIP_DNS` | 1 | STUN 服务器域名解析（推荐） |
| `LWIP_SO_REUSE` | 1 | SO_REUSEADDR（推荐） |
| `LWIP_SOCKET_SELECT` | 1 | select() 支持（默认开启） |

lwIP ≥ 2.1.0 的平台覆盖范围：

| 平台 | lwIP 版本 | 状态 |
|------|----------|------|
| ESP-IDF v4.x / v5.x | 2.1.2 / 2.1.3（espressif fork） | 主要目标 |
| Zephyr | 2.1.x（内置） | 支持 |
| RT-Thread | 2.1.x（软件包） | 支持 |
| STM32 CubeMX + FreeRTOS | 2.1.x（ST 分发） | 支持 |
| NuttX | POSIX 兼容 | 支持（通过原生 socket） |
| Linux（测试用） | 不适用（原生 socket） | 支持 |

### 5.3 应用层事件循环（模板代码）

```c
#include "nanortc.h"
#include <lwip/sockets.h>

static uint32_t get_millis(void) {
    // 平台相关的单调时钟
    // FreeRTOS: xTaskGetTickCount() * portTICK_PERIOD_MS
    // Zephyr:   k_uptime_get()
    // Linux:    clock_gettime(CLOCK_MONOTONIC)
    return platform_millis();
}

void nanortc_task(void *arg) {
    // 1. 初始化
    nanortc_t rtc;
    nanortc_config_t cfg = {
        .crypto = nanortc_crypto_mbedtls(),
        .role = NANORTC_ROLE_CONTROLLED,
    };
    nanortc_init(&rtc, &cfg);

    // 2. 创建 UDP socket（lwIP BSD socket API）
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(9999),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(fd, (struct sockaddr *)&local, sizeof(local));

    // 3. 添加本地候选
    char ip[16];
    get_local_ip(ip, sizeof(ip));  // 从 lwIP netif 读取
    nanortc_add_local_candidate(&rtc, ip, 9999);

    // 4. 信令: 通过已有通道交换 SDP
    char *remote_offer = my_signaling_recv_offer();
    char answer[2048];
    nanortc_accept_offer(&rtc, remote_offer, answer, sizeof(answer), NULL);
    my_signaling_send_answer(answer);

    // 5. 事件循环
    uint8_t buf[1500];
    for (;;) {
        nanortc_output_t out;
        uint32_t timeout_ms = 100;  // 默认值

        while (nanortc_poll_output(&rtc, &out)) {
            switch (out.type) {
            case NANORTC_OUTPUT_TRANSMIT:
                sendto(fd, out.transmit.data, out.transmit.len, 0,
                       (struct sockaddr *)&out.transmit.dest, sizeof(out.transmit.dest));
                break;

            case NANORTC_OUTPUT_EVENT:
                handle_event(&rtc, &out.event);
                break;

            case NANORTC_OUTPUT_TIMEOUT:
                timeout_ms = out.timeout_ms;
                goto wait;
            }
        }
    wait:;
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(fd, &rset);
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };

        int ret = select(fd + 1, &rset, NULL, NULL, &tv);
        uint32_t now = get_millis();

        if (ret > 0) {
            nanortc_addr_t src;
            socklen_t slen = sizeof(src.sa);
            int n = recvfrom(fd, buf, sizeof(buf), 0,
                            (struct sockaddr *)&src.sa, &slen);
            if (n > 0) {
                nanortc_handle_input(&rtc, &(nanortc_input_t){
                    .now_ms = now, .data = buf, .len = n, .src = src});
            }
        } else {
            nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now});
        }
    }
}

static void handle_event(nanortc_t *rtc, nanortc_event_t *evt) {
    switch (evt->type) {
    case NANORTC_EV_CONNECTED:
        printf("连接已建立\n");
        break;
    case NANORTC_EV_DATACHANNEL_OPEN:
        printf("DataChannel 已打开 (stream %d)\n", evt->datachannel_open.id);
        break;
    case NANORTC_EV_DATACHANNEL_DATA:
        if (evt->datachannel_data.binary) {
            process_data(evt->datachannel_data.data, evt->datachannel_data.len);
        } else {
            printf("收到: %.*s\n", (int)evt->datachannel_data.len,
                   (char *)evt->datachannel_data.data);
        }
        break;
#if NANORTC_HAVE_MEDIA_TRANSPORT
    case NANORTC_EV_MEDIA_DATA:
        if (evt->media_data.is_keyframe) { /* video keyframe */ }
        media_process(evt->media_data.data, evt->media_data.len,
                      evt->media_data.timestamp, evt->media_data.mid);
        break;
    case NANORTC_EV_KEYFRAME_REQUEST:
        send_keyframe(evt->keyframe_request.mid);
        break;
#endif
    case NANORTC_EV_DISCONNECTED:
        printf("对端断开连接\n");
        break;
    default:
        break;
    }
}
```

### 5.4 便利封装（可选）

不需要完全控制事件循环的用户可以使用 helper：

```c
// nano_helper.h — 可选，不属于核心库

typedef void (*nano_on_datachannel_data_fn)(uint16_t stream_id,
                                            const void *data, size_t len,
                                            void *userdata);
typedef void (*nano_on_audio_data_fn)(const void *data, size_t len,
                                      uint32_t timestamp, void *userdata);

typedef struct {
    uint16_t port;
    int task_priority;
    uint32_t task_stack_size;
    nano_on_datachannel_data_fn on_data;
    nano_on_audio_data_fn on_audio;    // DATA profile 时为 NULL
    void *userdata;
} nano_easy_config_t;

// 一键启动: 创建 socket、task 并运行事件循环
int nano_start_easy(nanortc_t *rtc, const nano_easy_config_t *easy_cfg);
void nano_stop_easy(nanortc_t *rtc);
```

---

## 6. 外部依赖

### 6.1 依赖矩阵

| 依赖 | 类型 | 被什么使用 | 说明 |
|------|------|-----------|------|
| **mbedtls** | 加密库（默认） | DTLS, HMAC-SHA1, CSPRNG, AES (SRTP) | ESP-IDF、Zephyr、RT-Thread、STM32 均内置。通过 `nanortc_crypto_provider_t` 抽象。编译时选择：`-DNANORTC_CRYPTO=mbedtls` |
| **OpenSSL** | 加密库（可选） | 同上 | Linux/macOS 主机开发。系统通常已安装。编译时选择：`-DNANORTC_CRYPTO=openssl` |
| **lwIP**（≥ 2.1.0） | TCP/IP 协议栈 | 仅应用层事件循环 | NanoRTC 核心**绝不**调用 lwIP。应用层使用 lwIP BSD socket API 进行 UDP I/O。 |

**不需要任何其他第三方库。** 不需要 usrsctp、libsrtp、libjuice、cJSON、plog。
所有协议逻辑都是自包含的。

### 6.2 自包含实现清单

| 组件 | 行数（预估） | 说明 |
|------|-------------|------|
| STUN 编解码 | ~400 | 核心（始终） |
| ICE agent | ~400 | 核心（始终） |
| SCTP-Lite | ~2500 | NANORTC_FEATURE_DATACHANNEL |
| SDP 解析/生成 | ~600-800 | 核心 + 按特性扩展 |
| DataChannel（DCEP） | ~400 | NANORTC_FEATURE_DATACHANNEL |
| CRC-32c | ~30 | NANORTC_FEATURE_DATACHANNEL |
| RTP 打包器 | ~500 | NANORTC_HAVE_MEDIA_TRANSPORT |
| RTCP | ~600 | NANORTC_HAVE_MEDIA_TRANSPORT |
| SRTP | ~400 | NANORTC_HAVE_MEDIA_TRANSPORT |
| Jitter Buffer | ~400 | NANORTC_FEATURE_AUDIO |
| 带宽估计 | ~300 | NANORTC_FEATURE_VIDEO |

总计：核心 ~1400 行，+DC ~4200 行，+音视频 ~6400 行。

---

## 7. 实施计划

### 7.1 阶段划分

当前执行状态以 `docs/PLANS.md`、`docs/QUALITY_SCORE.md` 和
`docs/exec-plans/**` 为准。本文只保留路线图摘要，避免与可执行计划重复。

| 阶段 | 状态 | 当前成果 / 下一步 |
|------|------|-------------------|
| Phase 0: Skeleton | 完成 | 仓库结构、CMake、公共头、crypto provider、Linux 合成测试环境。 |
| Phase 1: DataChannel E2E | 完成 | ICE / DTLS / SCTP / DCEP / SDP 打通；浏览器、ESP32-S3 与 libdatachannel 互通。 |
| Phase 2: Audio | Active | RTP / SRTP / RTCP / Jitter / SDP audio 已实现；剩余人工验证 ESP32 intercom 双向音频。 |
| Phase 3: Video | 完成 | H.264 FU-A、RTCP NACK/PLI、REMB/BWE、ESP32 camera 与浏览器验证。 |
| Phase 3.5: H.265/HEVC | Active | `nano_h265.c` + 单元测试已具备；剩余 SDP/RTC wiring、interop、浏览器验证。 |
| Phase 4: Quality | 完成 | Unity、覆盖率门槛、fuzz harness、CI 约束和 interop 框架。 |
| Phase 5: Network Traversal | Active | STUN srflx、TURN Allocate/Refresh/Permission/ChannelBind/ChannelData 已实现；继续补 relay-only 自动化覆盖。 |
| Phase 6-8: Resource / Stability | Active | RAM 优化、hot path hardening、ICE pending table、video pkt_ring、H.264 zero-copy；剩余 IoT profile 与可选 P2 项。 |
| Phase 9: BWE Perception | Active | TWCC parser、SDP/RTP extmap、loss-based BWE、runtime tunables、track stats 与 example coordinator。 |
| Phase 10+ 候选 | Draft | TWCC feedback sending、API 边界收敛、RTC 编排拆分、进一步 zero-copy。 |

### 7.2 测试策略

NanoRTC 是 Sans I/O 架构，每个模块都可以**无需网络**进行单元测试：

```c
// 示例: 在内存中完整测试 SCTP 握手

void test_sctp_handshake(void) {
    nanortc_t server, client;
    // ... 用 crypto provider 初始化两端

    // 客户端生成 INIT
    nanortc_output_t out;
    nanortc_handle_input(&client, &(nanortc_input_t){.now_ms = 0});
    nanortc_poll_output(&client, &out);
    assert(out.type == NANORTC_OUTPUT_TRANSMIT);

    // 将 INIT 送给服务端
    nanortc_handle_input(&server, &(nanortc_input_t){
        .now_ms = 0, .data = out.transmit.data, .len = out.transmit.len, .src = fake_addr});

    // 服务端生成 INIT-ACK
    nanortc_poll_output(&server, &out);
    assert(out.type == NANORTC_OUTPUT_TRANSMIT);

    // 继续: COOKIE-ECHO → COOKIE-ACK → ESTABLISHED
    // 未打开任何 socket。不需要网络。运行耗时 < 1ms。
}
```

与真实浏览器的集成测试使用 Linux 测试环境，配合实际 UDP socket 和简单的
HTTP 信令适配层。

### 7.3 关键 RFC 参考

| 模块 | RFC | 标题 |
|------|-----|------|
| ICE | RFC 8445 | 交互式连接建立 |
| STUN | RFC 8489 | NAT 会话穿越工具 |
| DTLS | RFC 6347 | 数据报传输层安全 1.2 |
| SCTP | RFC 4960 | 流控制传输协议 |
| SCTP-over-DTLS | RFC 8261 | SCTP 的数据报传输层安全 |
| PR-SCTP | RFC 3758 | 部分可靠性扩展 |
| DataChannel 传输 | RFC 8831 | WebRTC 数据通道 |
| DCEP | RFC 8832 | WebRTC 数据通道建立协议 |
| RTP | RFC 3550 | 实时传输协议 |
| SRTP | RFC 3711 | 安全实时传输协议 |
| SDP | RFC 8866 | 会话描述协议 |
| JSEP | RFC 8829 | JavaScript 会话建立协议 |
| WebRTC 安全架构 | RFC 8827 | WebRTC 安全架构 |
| 复用方案 | RFC 7983 | DTLS-SRTP 复用方案更新 |

### 7.4 AI 辅助开发指南（Claude Code）

使用 AI 编码代理开发 NanoRTC 时遵循以下规范（参见 `AGENTS.md`）：

1. **逐模块实现**：一次只实现一个模块。每个模块应能独立编译并通过单元测试，
   再进行集成。

2. **RFC 为准绳**：每个协议模块以对应的 RFC 为准，包括包格式、状态机和行为要求。

3. **Sans I/O 纪律**：`src/` 目录内禁止添加 `#include <sys/socket.h>`、
   `#include <pthread.h>`、`#include <time.h>` 或任何 OS/平台头文件。
   唯一允许的外部 include：
   - `<string.h>`, `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`（C 标准）
   - `"nanortc_crypto.h"`（crypto provider 接口）
   - 不允许其他外部头文件。

4. **内存纪律**：优先使用栈分配和调用者提供的缓冲区。如果不可避免需要动态分配，
   使用可配置的分配器：
   ```c
   void *nano_alloc(size_t size);   // 用户可覆盖
   void  nano_free(void *ptr);
   ```

5. **解析器优先写测试**：STUN、SDP、SCTP chunk 编解码应以测试驱动开发，
   使用从浏览器流量捕获的已知正确字节序列作为测试数据。

6. **特性标志守卫**：使用正交特性标志：`#if NANORTC_FEATURE_DATACHANNEL`（SCTP/DC）、
   `#if NANORTC_HAVE_MEDIA_TRANSPORT`（RTP/SRTP）、`#if NANORTC_FEATURE_AUDIO`、`#if NANORTC_FEATURE_VIDEO`。

7. **无全局状态**：所有状态存在于 `nanortc_t` 内部。多个 `nanortc_t` 实例
   必须能独立共存。

8. **命名规范**：所有公共符号以 `nano_` 为前缀。内部函数使用模块前缀
   （如 `stun_`、`sctp_`、`dtls_`）。

9. **错误处理**：函数返回 `int`（0 = 成功，负数 = 错误）。错误码在 `nanortc.h`
   中定义。库代码中禁止使用 `assert()`，应返回错误码。

10. **字节序**：网络字节序转换使用自包含的内联函数，不使用平台头文件中的
    `htons()`/`ntohs()`：
    ```c
    static inline uint16_t nanortc_htons(uint16_t x) {
        return (x >> 8) | (x << 8);
    }
    ```

---

## 8. 相关项目与参考

| 项目 | 语言 | I/O 模型 | 对 NanoRTC 的借鉴意义 |
|------|------|----------|----------------------|
| [str0m](https://github.com/algesten/str0m) | Rust | **Sans I/O** | 架构灵感：poll/handle 模式、无内部线程、时间作为外部输入 |
| [libdatachannel](https://github.com/paullouisageneau/libdatachannel) | C++ | 内部 I/O | 互通测试夹具；不作为 wire format / 状态机实现来源 |
| [libjuice](https://github.com/paullouisageneau/libjuice) | C | 内部 I/O | ICE/STUN/TURN 工程背景；协议行为仍以 RFC 为准 |
| [esp_peer](https://components.espressif.com/components/espressif/esp_peer) | C | 内部 I/O | Espressif 官方 WebRTC 组件；展示了 ESP32 优化模式（闭源） |
| [usrsctp](https://github.com/sctplab/usrsctp) | C | 内部 I/O | SCTP 工程背景；协议实现以 RFC 4960 / 8831 / 8832 为准 |

---

## 9. 许可证

NanoRTC 采用 **MIT License** 发布。

- 兼容所有商业和开源使用
- 无 copyleft 义务，适合产品集成
- mbedtls（Apache 2.0）与 MIT 兼容

---

*文档版本: 1.0*
*最后更新: 2026-03-26*
*作者: Bin (0x1abin) + Claude 架构分析*
