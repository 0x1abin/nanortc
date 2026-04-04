# ESP32-P4 Camera — WebRTC 实时推流

ESP32-P4 通过 MIPI CSI 采集摄像头画面、I2S 采集 ES8311 麦克风音频，
经硬件 H.264 + Opus 编码后，使用 NanoRTC 以 WebRTC 协议实时推送到浏览器。

## 硬件要求

| 项目 | 规格 |
|------|------|
| 开发板 | ESP32-P4-Nano v1.0（或任何带 CSI 摄像头 + I2S 音频 codec 的 ESP32-P4 板） |
| SoC | ESP32-P4，双核 RISC-V @ 360 MHz，硬件 H.264 编码器 |
| PSRAM | 必需（建议 32 MB） |
| 网络 | 以太网（ESP32-P4 无 WiFi） |
| 摄像头 | OV5647 (MIPI CSI) — 可通过 board YAML 配置其他传感器 |
| 音频 | ES8311 codec (I2S) — 可通过 board YAML 配置其他 codec |

硬件引脚定义在 `boards/<board>/board_peripherals.yaml` 中，
应用代码不硬编码任何引脚。参见[自定义板适配](#自定义板适配)了解如何适配你的硬件。

## 编译与烧录

```bash
cd examples/esp32_camera

# 1. 设置目标芯片（会下载依赖组件）
idf.py set-target esp32p4

# 2. 生成板级配置（首次或切换板型时执行）
python managed_components/espressif__esp_board_manager/gen_bmgr_config_codes.py \
  -b esp32_p4_nano -c boards

# 3. 编译并烧录
idf.py build
idf.py flash monitor
```

步骤 2 完成后，后续修改代码只需 `idf.py build`。

## 使用方法

1. 烧录后等待设备获取 IP 地址（串口日志会打印）
2. 在浏览器打开 `http://<device-ip>/`（推荐 Chrome）
3. 点击 **Connect**
4. 实时音视频流将显示在页面上

## 架构

```
Core 1 — camera_task (pri 6)               Core 0 — webrtc_task (pri 5)
┌─────────────────────────────┐            ┌─────────────────────────────┐
│ V4L2 DQBUF (YUV420)         │            │ nano_run_loop_step()        │
│         ↓                   │  FreeRTOS  │   ↑ DTLS/STUN/RTP polling   │
│ H.264 HW encode             │───Queue───→│ nanortc_send_video()        │
│         ↓                   │  (depth=2) │ nanortc_send_audio()        │
│ V4L2 QBUF (return buffer)   │            │   ↓ RTP/SRTP → UDP          │
└─────────────────────────────┘            └─────────────────────────────┘

Core 0 — microphone_task (pri 7)
┌──────────────────────────────┐
│ esp_capture (ES8311 → Opus)  │───Queue (depth=4)───→ webrtc_task
└──────────────────────────────┘
```

- **板级初始化**: `esp_board_manager` 处理 I2C、I2S、XCLK、LDO、codec、摄像头传感器
- **摄像头采集**: 通过 board manager 提供的 `dev_path` 使用 V4L2 API，MMAP 双缓冲
- **H.264 编码**: 硬件编码器（仅 ESP32-P4），Annex-B 输出，支持动态关键帧/码率
- **音频采集**: `esp_capture` 框架 + Opus 编码，codec 由 board manager 管理
- **WebRTC**: NanoRTC (Sans I/O) + mbedTLS (DTLS-SRTP)
- **信令**: HTTP POST `/offer` (SDP offer/answer)

## 自定义板适配

应用代码通过 `esp_board_manager` handle 访问硬件——**没有硬编码任何引脚或芯片型号**。
适配其他 ESP32-P4 开发板：

1. 在 `boards/` 下创建新目录（可复制 `boards/esp32_p4_nano/`）：

   | 文件 | 用途 |
   |------|------|
   | `board_info.yaml` | 板名、芯片类型 |
   | `board_peripherals.yaml` | I2C / I2S / GPIO / LDO 引脚配置 |
   | `board_devices.yaml` | 摄像头、音频 codec、SD 卡设备配置 |
   | `sdkconfig.defaults.board` | 摄像头传感器驱动配置（如 OV5647、SC2336） |

2. 生成并编译：
   ```bash
   python managed_components/espressif__esp_board_manager/gen_bmgr_config_codes.py \
     -b my_board -c boards
   idf.py build
   ```

### YAML 配置指南

| 你的板子有... | 修改... |
|--------------|---------|
| 不同的 I2C/I2S 引脚 | `board_peripherals.yaml` |
| 不同的摄像头传感器 | `board_devices.yaml`（sub_type, xclk）+ `sdkconfig.defaults.board` |
| 不同的音频 codec（ES8388 等）| `board_devices.yaml`（chip 字段） |
| 无功放 | 从 peripherals 和 device 引用中删除 `gpio_pa_control` |
| DVP 摄像头而非 CSI | 改 `sub_type: "dvp"`，删除 `ldo_mipi` |

### 约束条件

- **仅支持 ESP32-P4**：硬件 H.264 编码器是 P4 专有的
- **需要 PSRAM**：Opus 编码器需要 40 KB+ 栈空间，摄像头帧缓冲使用 PSRAM
- **CSI 摄像头需要 LDO_MIPI**：`main.c` 中调用 `esp_board_periph_init(LDO_MIPI)`，DVP/SPI 摄像头可注释此行

## 参数配置

通过 `idf.py menuconfig` → "ESP32 Camera Example" 调整：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `EXAMPLE_UDP_PORT` | 9999 | WebRTC STUN/DTLS/RTP 端口 |
| `EXAMPLE_VIDEO_WIDTH` | 1280 | 视频宽度（需匹配传感器模式） |
| `EXAMPLE_VIDEO_HEIGHT` | 960 | 视频高度（需匹配传感器模式） |
| `EXAMPLE_VIDEO_FPS` | 30 | 视频帧率 |
| `EXAMPLE_H264_BITRATE_KBPS` | 1024 | H.264 目标码率 (kbps) |
| `EXAMPLE_H264_GOP` | 60 | 关键帧间隔（30fps 下 GOP=60 = 2 秒） |

ESP32-P4-Nano 板在 `sdkconfig.defaults.esp32p4` 中覆盖分辨率为 1920x1080。

## 关键帧处理

- **定时 IDR**: 每 GOP 帧自动生成
- **连接时 IDR**: WebRTC 连接建立后立即强制输出
- **PLI 响应**: 浏览器 RTCP PLI → `NANORTC_EV_KEYFRAME_REQUEST` → 编码器 IDR

## 文件结构

```
esp32_camera/
├── CMakeLists.txt                  # 项目配置（包含 board_manager.defaults）
├── partitions.csv                  # Flash 分区表
├── sdkconfig.defaults              # 通用配置（mbedTLS, lwIP, nanortc）
├── sdkconfig.defaults.esp32p4      # P4 专属（SPIRAM, 以太网, 分辨率）
├── boards/
│   └── esp32_p4_nano/              # 板级配置（YAML，见自定义板适配）
│       ├── board_info.yaml
│       ├── board_peripherals.yaml
│       ├── board_devices.yaml
│       └── sdkconfig.defaults.board
├── components/
│   └── gen_bmgr_codes/             # gen_bmgr_config_codes.py 生成（不入 git）
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild           # Menuconfig：视频/编码器参数
│   ├── idf_component.yml           # 依赖：esp_board_manager, esp_capture 等
│   ├── main.c                      # 板级初始化、WebRTC 会话、任务编排
│   ├── camera.c / camera.h         # V4L2 采集（使用 board manager dev_path）
│   ├── encoder.c / encoder.h       # H.264 硬件编码器（关键帧 + 码率控制）
│   ├── microphone.c / microphone.h # Opus 采集（使用 board manager codec_dev）
│   └── index.html                  # 浏览器 WebRTC 界面
├── README.md                       # English documentation
└── README_CN.md                    # 本文件
```

## 已知限制

- 仅支持以太网（ESP32-P4 无 WiFi）
- 仅支持单客户端同时连接
- `VIDIOC_S_PARM` 帧率设置在当前 esp_video 版本不生效，使用传感器默认帧率
