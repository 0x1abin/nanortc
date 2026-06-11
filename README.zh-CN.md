# NanoRTC

[English](README.md) | 简体中文

一个面向 RTOS 与嵌入式系统的 Sans I/O 纯 C WebRTC 实现。

> **AI 原生实现**:本仓库的每一行代码——库源码、测试、构建系统、CI、文档与示例——均由 AI 编码智能体编写。人类负责架构决策与正确性验证,智能体负责执行。详见[项目如何构建](#项目如何构建)。

## 什么是 NanoRTC?

NanoRTC 是一个从零设计的 WebRTC 协议栈,目标是运行 FreeRTOS、Zephyr、RT-Thread 等 RTOS 的资源受限微控制器。

**Sans I/O 架构** —— 受 [str0m](https://github.com/algesten/str0m)(Rust)启发,NanoRTC 是一个纯状态机。它从不触碰套接字、线程、内存分配或时钟。事件循环与所有 I/O 都由你的应用程序掌控。这使得 NanoRTC 可移植到任何平台,且无需网络即可测试。

```
                     ┌─────────────────────────┐
  UDP bytes ────────►│                         │──────► bytes to send
  monotonic time ───►│  nanortc_t              │──────► application events
  user commands ────►│  (pure state machine)   │──────► next timeout (ms)
                     │                         │
                     │  No sockets. No threads.│
                     │  No malloc. No clocks.  │
                     └─────────────────────────┘
```

## 特性

- **正交的特性开关** —— 只编入你需要的部分:

| 配置 | Flash (.text) | RAM (sizeof) | 开关 |
|--------------|---------------|-------------|-------|
| 仅核心 | 29.0 KB | 10.2 KB | DC=OFF AUDIO=OFF VIDEO=OFF |
| DataChannel | 38.8 KB | 19.4 KB | DC=ON |
| 仅音频 | 40.8 KB | 20.6 KB | DC=OFF AUDIO=ON |
| DataChannel + 音频 | 50.6 KB | 29.9 KB | DC=ON AUDIO=ON |
| 仅媒体(无 DC) | 45.3 KB | 51.0 KB | DC=OFF AUDIO=ON VIDEO=ON |
| 完整媒体 | 55.0 KB | 60.3 KB | DC=ON AUDIO=ON VIDEO=ON |

> 实测平台:ESP32-P4(RISC-V HP),ESP-IDF 5.5 mbedTLS,`-Os`(`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`)。`sizeof(nanortc_t)` 即单连接的全部 RAM 占用——没有堆分配。Flash 数字只统计 nanortc 库自身代码(`libnanortc.a` .text);mbedTLS 与 lwIP 独立计算,通常与固件其余部分共享。
> 以上尺寸基于 ESP-IDF Kconfig 默认值——内置 IoT 级缓冲区/队列配置,完整 ICE 栈不打折(TURN 中继、srflx 发现、IPv6 host 候选、TWCC/BWE 感知、RFC 8445 加固)。可用 `./scripts/measure-sizes.sh --esp32 esp32p4` 复现;进一步裁剪见 `idf.py menuconfig` 或 [`NANORTC_CONFIG_FILE`](docs/engineering/memory-profiles.md)。

任意组合均可工作——只要音频不要 DataChannel、只要视频不要音频,都没问题。

- **ICE** —— 受控(answerer)与主控(offerer)角色、trickle ICE、ICE restart
- **DTLS 1.2** —— 通过可插拔加密后端(mbedtls 或 OpenSSL)
- **SCTP** —— 面向 WebRTC DataChannel 的最小子集(可靠 + 不可靠传输)
- **DataChannel** —— DCEP 协议,支持可靠与不可靠模式
- **RTP/RTCP/SRTP** —— 音视频媒体传输,H.264 FU-A 与 H.265(RFC 7798)分包
- **SDP** —— Offer/Answer 协商,多轨媒体
- **NAT 穿透** —— STUN 服务器反射地址发现 + TURN 中继客户端(可选,`NANORTC_FEATURE_TURN`)
- **带宽估计** —— REMB + TWCC 丢包感知的接收端 BWE,支持视频自适应
- **唯一外部依赖** —— 只有 mbedtls(ESP-IDF、Zephyr、RT-Thread、STM32 均内置)

## 快速开始

```bash
# Build (Linux/macOS) — default: DataChannel only
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Enable audio + video
cmake -B build -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON

# With OpenSSL (for Linux host development)
cmake -B build -DNANORTC_CRYPTO=openssl

# Build examples (full media)
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON \
      -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON
cmake --build build

# ESP-IDF
idf.py build
```

## 使用方式

> 下方函数签名为便于阅读做了简化——省略了可选的出参。完整 API 见 [include/nanortc.h](include/nanortc.h)。

**配置、协商、驱动:**

```c
#include "nanortc.h"

nanortc_t rtc;
nanortc_init(&rtc, &(nanortc_config_t){
    .crypto = nanortc_crypto_mbedtls(),   // or nanortc_crypto_openssl()
    .role   = NANORTC_ROLE_CONTROLLED,    // or _CONTROLLING to offer
});
nanortc_add_local_candidate(&rtc, local_ip, local_port);

char sdp[4096];
nanortc_accept_offer(&rtc, remote_offer, sdp);
// Offerer: create_datachannel("chat") → create_offer(sdp) → accept_answer(remote_answer)
```

事件循环是对称的——**排空输出,喂入输入。** 套接字与时钟由你掌控:

```c
for (;;) {
    nanortc_output_t out;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK)
        handle_output(&out);              // send UDP, fire app event, note next wake-up

    size_t len = recv_udp(fd, buf, sizeof buf, &src, wake_ms);
    nanortc_handle_input(&rtc, &(nanortc_input_t){
        .now_ms = now_ms(), .data = buf, .len = len, .src = src,
    });
}
```

一个输入结构体进,一个输出结构体出。没有隐藏状态,没有后台线程。

完整可运行示例——浏览器互通、macOS 摄像头推流、ESP32 DataChannel——见 [examples/](examples/)。

## 平台支持

| 平台 | 状态 | 说明 |
|----------|--------|-------|
| Linux / macOS | 宿主机开发与测试 | OpenSSL 或 mbedtls |
| ESP-IDF (ESP32) | 首要嵌入式目标 | 内置 mbedtls、lwIP |
| Zephyr | 已支持 | 内置 mbedtls、lwIP |
| RT-Thread | 已支持 | mbedtls 软件包、lwIP |
| STM32 + FreeRTOS | 已支持 | ST 发行版 mbedtls、lwIP |
| NuttX | 已支持 | POSIX 兼容套接字 |

## 项目结构

```
include/nanortc.h          Single public API header
src/                        Protocol modules (Sans I/O, no platform deps)
crypto/                     Pluggable crypto providers (mbedtls, openssl)
tests/                      Unit tests + end-to-end tests (no network needed)
tests/interop/              Interop tests against libdatachannel (C++)
examples/                   Application templates
  common/                   Reusable event loop, signaling, media source
  browser_interop/          DataChannel + media browser harness
  macos_camera/             macOS camera/mic → browser streaming
  esp32_{datachannel,audio,video,camera}/   ESP-IDF targets
  rk3588_uvc_camera/        RK3588 UVC camera demo
  tools/                    Dev utilities
  sample_data/              Media samples (git submodule)
docs/                       Design docs, execution plans, engineering standards
```

模块依赖图与数据流见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 项目如何构建

NanoRTC 是一次 **AI 原生软件工程**实验,受 [Harness Engineering](https://openai.com/index/harness-engineering/) 启发。整个代码库由 AI 编码智能体生成,遵循一条原则:**人类掌舵,智能体执行**。

具体落地为:

- **架构与设计** —— 由人类决策,沉淀在 `docs/design-docs/`
- **全部代码** —— 由 AI 智能体编写:库源码、测试、CI、构建系统、文档
- **质量门禁** —— 通过 CI 机械化强制执行:禁用头文件检查、禁止 malloc、符号命名、格式检查、7 种特性组合构建矩阵、AddressSanitizer
- **RFC 合规** —— 协议实现以 RFC 为唯一权威标准,而非参考第三方代码
- **持续验证** —— `./scripts/ci-check.sh` 在本地运行与 GitHub Actions 相同的检查。自动探测 `ccache`,跨次运行保留构建目录以增量编译,并提供 `--fast` 用于推送前的紧凑循环(跳过低收益组合与 libdatachannel 互通套件——秒级而非分钟级)

仓库结构本身就是为智能体可读性设计的:[AGENTS.md](AGENTS.md) 作为入口,逐级展开到更深层的文档。约束由代码强制执行,而不靠约定俗成。

## 文档

| 文档 | 说明 |
|----------|------------|
| [AGENTS.md](AGENTS.md) | 智能体入口——构建命令、强制规则 |
| [构建指南](docs/guide-docs/build.md) | 构建命令、特性开关、模糊测试、覆盖率、ESP-IDF |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 模块依赖图、分层模型、数据流 |
| [设计规格](docs/design-docs/nanortc-design-draft.md) | 完整的权威设计参考 |
| [核心信念](docs/design-docs/core-beliefs.md) | 不可妥协的设计原则 |
| [执行计划](docs/PLANS.md) | 进行中与已完成的实现计划 |
| [质量评分](docs/QUALITY_SCORE.md) | 各模块质量等级 |
| [内存画像](docs/engineering/memory-profiles.md) | 各配置的 RAM 用量与调优指南 |
| [编码标准](docs/engineering/coding-standards.md) | 命名、风格、RFC 测试要求 |
| [Safe C 准则](docs/engineering/safe-c-guidelines.md) | 禁用函数、缓冲区安全规则 |
| [RFC 索引](docs/references/rfc-index.md) | 协议规范引用 |

## 贡献

NanoRTC 正在积极开发中。核心协议栈——DataChannel、音频、视频/H.264/H.265、符合 RFC 8445 的 ICE+STUN+TURN、SRTP 以及 TWCC/BWE 感知——已完成编码,并通过了与 libdatachannel 和 Chromium 的互通验证。Phase 8 持续优化与 Phase 9 BWE 感知正在推进;当前阶段状态见 [docs/PLANS.md](docs/PLANS.md)。全部 22 个库模块均为 A 级——经过模糊测试、浏览器验证、libdatachannel 互通验证,覆盖率 80% 以上。

欢迎贡献。提交变更前请先阅读 [AGENTS.md](AGENTS.md) 了解构建说明与强制规则。

## 许可证

[MIT](LICENSE)
