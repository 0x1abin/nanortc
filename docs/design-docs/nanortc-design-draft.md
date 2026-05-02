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


NanoRTC 不持有 socket、线程或时钟,因此每个模块都可以在内存中以合成时间戳完整测试,无需真实网络。集成测试通过 Linux 主机注入真实 UDP 与浏览器互通验证。

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
├── include/                  // 公共头(单一文件 nanortc.h)
├── src/                      // Sans I/O 协议实现
└── crypto/                   // 可插拔 crypto provider 接口与默认实现
```

完整模块依赖图、协议层目录结构、examples/ 与 tests/ 布局见 `ARCHITECTURE.md`。


### 2.5 条件编译

协议模块的导入由 `#if NANORTC_FEATURE_*` 包裹(具体规则见 `include/nanortc_config.h`)。SDP 生成根据相同标志自适应追加 `m=application` / `m=audio` / `m=video` 行。正交标志可任意组合,CI 矩阵覆盖 7 种 canonical 组合。

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


### 3.3 DTLS 1.2（nano_dtls.c）

**范围**：DTLS 1.2 握手和记录层，委托给 crypto provider。

NanoRTC 不自行实现 DTLS。crypto provider（通常是 mbedtls）处理完整的 DTLS 状态机。
NanoRTC 提供 BIO（缓冲 I/O）接口：


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


### 3.5 DataChannel（nano_datachannel.c）

**范围**：WebRTC 数据通道建立协议（DCEP，RFC 8832）。

- DATA_CHANNEL_OPEN 消息解析/生成
- DATA_CHANNEL_ACK 响应
- 通道类型映射：可靠、不可靠（最大重传次数 / 最大生命周期）
- 有序 vs 无序交付（映射到 SCTP 流参数）
- 字符串 vs 二进制消息类型（PPID: 51 为字符串, 53 为二进制）
- 多个 DataChannel 使用不同 SCTP stream ID 并发


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

**不做：** 发送 TWCC feedback 包（接收端需要，后续 phase 候选）、GCC/trendline 延迟控制、pacer、独立 NACK/PLI/FIR 事件（当前 `NANORTC_EV_KEYFRAME_REQUEST` 足够）。

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


---

## 4. Crypto Provider 接口

### 4.1 设计

NanoRTC 将所有加密操作抽象在可插拔的 provider 接口后面。用户可以将 mbedtls
替换为 wolfSSL、BearSSL、OpenSSL、硬件加密加速器或平台特定实现，无需修改 NanoRTC 核心代码。
Linux 主机开发可使用 OpenSSL 后端（`-DNANORTC_CRYPTO=openssl`）。

**必需能力(CORE_ONLY profile 即需要):**

- DTLS 1.2 全生命周期:init / handshake / encrypt / decrypt / export_keying_material / get_fingerprint / set_role
- HMAC-SHA1 —— STUN MESSAGE-INTEGRITY
- CSPRNG —— ICE 凭据、SCTP cookie、DTLS nonce

**MEDIA profile 追加:**

- AES-128-CM —— SRTP 加解密
- HMAC-SHA1-80 —— SRTP 认证标签

**TURN profile 追加:**

- MD5 —— RFC 8489 §9.2.2 长期凭据派生

接口签名见 `crypto/nanortc_crypto.h`。默认实现为 mbedtls(嵌入式平台内置);OpenSSL 后端用于 Linux 主机开发。可替换为 wolfSSL、BearSSL、硬件加速器或平台特定实现而无需改动核心代码。


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


---

## 7. 进一步阅读

| 主题 | 文档 |
|------|------|
| 模块依赖图 + 包生命周期 | `ARCHITECTURE.md` |
| Phase 状态与执行计划 | `docs/PLANS.md` + `docs/exec-plans/active/*` |
| 模块质量评分 | `docs/QUALITY_SCORE.md` |
| RFC 章节级符合性 | `docs/references/rfc-index.md`、`docs/engineering/ice-rfc-compliance.md`、`docs/engineering/turn-rfc-compliance.md` |
| 编码标准与 Sans I/O 红线 | `AGENTS.md`、`docs/engineering/coding-standards.md` |
| 内存与构建 profile | `docs/engineering/memory-profiles.md`、`docs/guide-docs/build.md` |
| 应用层事件循环参考实现 | `examples/common/run_loop_linux.c`、`examples/browser_interop/main.c` |
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
