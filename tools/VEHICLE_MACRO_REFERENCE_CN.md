# Vehicle 双核工程业务宏参考

更新日期：2026-08-16

## 1. 文档范围

本文记录 `RA8P1_DualCoreProject` 中手写业务代码里需要调试、标定或维护的宏，重点覆盖 CPU0 的摄像头、YOLO、显示、防跌落和 IPC，以及 CPU1 的安全仲裁、电机、无线与 Wi-Fi 业务。

以下内容不逐项列出：FSP/Smart Configurator 生成文件、`ra_gen`、CMSIS、SEGGER RTT、JPEG/第三方库内部宏、NPU 自动生成模型张量宏、头文件保护宏、传感器寄存器地址和 nRF24 驱动寄存器位定义。这些不是日常业务调参入口，随意修改可能破坏生成代码或底层协议。

风险等级说明：

| 等级 | 含义 |
|---|---|
| A：普通调试 | 可以按需要修改，但仍需重新编译并观察日志或输出。 |
| B：实车标定 | 会改变运动、超时或传输性能，必须架空车轮或在受控环境验证。 |
| C：离线回归 | 会改变视觉模型输入或安全判断，修改后必须重跑离线回放和分组验证。 |
| D：双端协议 | CPU0、CPU1 或遥控端必须保持一致，禁止只改一端。 |
| E：硬件/FSP | 涉及地址、尺寸、摄像头时序或外设能力，需同时核对 FSP 配置和硬件手册。 |

## 2. YOLO 安全链路

```text
CPU0 128x128 YOLO 推理
    -> 后处理候选门限和 NMS
    -> 置信度严格大于 60% 时发布 positive
    -> CPU0/CPU1 YOLO IPC
    -> CPU1 对最近6个不同源帧结果执行3/6投票
    -> 命中时 STOP，并锁定 MANUAL
    -> 遥控端重新下发 AUTO 后才解除锁定
```

### 2.1 CPU0 YOLO 推理与后处理

文件：`Renesas_Cup_Vehicle_CPU0/src/ai_thread_entry.c`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `AI_CONFIDENCE_THRESHOLD` | `0.50f` | YOLO 解码的最低候选置信度，同时影响检测框、JPEG 事件和能否进入后续安全判断。 | C |
| `AI_SAFETY_CONFIDENCE_THRESHOLD` | `0.60f` | CPU0 发布 YOLO 安全阳性结果的门限；代码使用 `max_confidence > threshold`，因此恰好 60% 不算阳性。 | C |
| `AI_NMS_IOU_THRESHOLD` | `0.45f` | NMS 重叠框抑制的 IoU 门限。 | C |
| `AI_JPEG_CLEAR_FRAME_COUNT` | `10U` | 检测事件后需要连续多少个干净帧，才允许下一次 JPEG 事件重新武装。 | A |

注意：安全门限不能有效低于候选门限。若 `AI_SAFETY_CONFIDENCE_THRESHOLD` 设为 `0.40f`，低于 50% 的框已经在解码阶段被丢弃，CPU1 仍收不到它。

文件：`Renesas_Cup_Vehicle_CPU0/src/AI/yolo_postprocess.h`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `YOLO_INPUT_SIZE` | `128` | 模型输入边长。 | C/E |
| `YOLO_CLASS_COUNT` | `1` | 模型类别数，本工程只识别一种目标。 | C |
| `YOLO_OUTPUT_BOX_COUNT` | `336` | 模型输出候选框数量，必须与模型张量一致。 | C |
| `YOLO_OUTPUT_ATTRS` | `5` | 每个候选的输出属性数，必须与导出模型一致。 | C |
| `YOLO_MAX_DETECTIONS` | `4` | NMS 后最多保留的检测数量。 | A/C |

### 2.2 CPU0 YOLO 输入裁剪与缩放

文件：`Renesas_Cup_Vehicle_CPU0/src/AI/ai_preprocess.h`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `AI_PREPROCESS_SOURCE_WIDTH` / `HEIGHT` | `1024U` / `600U` | VIN RGB565 源帧尺寸。 | E |
| `AI_PREPROCESS_SOURCE_STRIDE_BYTES` | `2048U` | 每行字节跨度，当前为 `1024 x 2`。 | E |
| `AI_PREPROCESS_CROP_X` / `Y` | `212U` / `0U` | 从源帧左上角开始的 YOLO 方形裁剪坐标。 | C/E |
| `AI_PREPROCESS_CROP_WIDTH` / `HEIGHT` | `600U` / `600U` | 送入缩放器的裁剪区域。 | C/E |
| `AI_PREPROCESS_DESTINATION_WIDTH` / `HEIGHT` | `128U` / `128U` | 模型输入目标尺寸。 | C/E |
| `AI_PREPROCESS_CHANNEL_COUNT` | `3U` | 模型输入通道数。 | C |
| `AI_PREPROCESS_PIXELS_PER_VECTOR` | `8U` | Helium 预处理一次处理的像素数量。 | E |

### 2.3 CPU1 YOLO 安全仲裁

文件：`Renesas_Cup_Vehicle_CPU1/src/vehicle_thread_entry.c`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `VEHICLE_YOLO_SAFETY_ENABLE` | `1U` | YOLO 安全仲裁总开关。`1U` 启用纯 3/6 停车；`0U` 禁止 YOLO 改变电机/模式，但仍读取并排空 IPC 队列。 | C |
| `VEHICLE_YOLO_VOTE_WINDOW_RESULTS` | `6U` | 最多保留 6 个不同源帧结果。 | C |
| `VEHICLE_YOLO_REQUIRED_POSITIVES` | `3U` | 窗口内至少 3 个阳性才锁停。 | C |

关闭 YOLO 安全仲裁时只改：

```c
#define VEHICLE_YOLO_SAFETY_ENABLE (0U)
```

这不会关闭 CPU0 推理，也不会关闭 YOLO IPC 发送。重新打开时改回 `1U` 并重新编译 CPU1。

## 3. 黑白防跌落与自主脱险

文件：`Renesas_Cup_Vehicle_CPU0/src/navigation_thread_entry.c`

### 3.1 输入、定点模型和确认帧

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `NAV_GRAY_WIDTH` / `HEIGHT` | `200U` / `112U` | Gray8 输入尺寸。 | C/E |
| `NAV_ROI_HEIGHT` | `24U` | 使用图像底部 24 行作为防跌落 ROI。 | C |
| `NAV_GRAY_THRESHOLD` | `128U` | 小于该灰度值记为暗像素。 | C |
| `NAV_MODEL_FEATURE_COUNT` | `24U` | 固件定点线性模型的特征数量。 | C |
| `NAV_MODEL_INPUT_FRACTION_BITS` | `4U` | 输入特征 Q 格式的小数位数。 | C |
| `NAV_MODEL_SCORE_FRACTION_BITS` | `18U` | 模型分数 Q 格式的小数位数。 | C |
| `NAV_MODEL_INTERCEPT_Q` | `1624267` | 定点线性危险评分器截距。 | C |
| `NAV_MODEL_STOP_THRESHOLD_Q` | `-237271` | STOP 决策门限；数值越小越容易停车。 | C |
| `NAV_STOP_CONFIRM_FRAMES` | `1U` | 连续危险多少帧后确认 STOP。 | C |
| `NAV_ALLOW_CONFIRM_FRAMES` | `3U` | 连续安全多少帧后恢复动作许可。 | C |

`NAV_MODEL_INTERCEPT_Q`、模型权重、特征顺序和两个 Q 格式构成同一个定点模型，不能单独凭感觉修改。任何模型相关变化都必须先通过 PC 离线回放，再做 MCU 与 PC 逐样本一致性对比。

### 3.2 脱险转向

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `NAV_ESCAPE_TURN_TOWARD_DARK_SUPPORT` | `1U` | 左右支撑差足够明显时优先朝暗支撑更多的一侧转。 | C |
| `NAV_ESCAPE_SUPPORT_DELTA_Q` | `80` | 左右支撑差门限，F4 格式，80 约为 5 个百分点。 | C |
| `NAV_ESCAPE_SAFE_MARGIN_Q` | `131072` | 仅用于转动中判断是否提前停轮的额外安全分数，F18 格式约为 0.5 logit；停稳后按正常 SAFE 门限复核。 | C |
| `NAV_ESCAPE_YOLO_WAIT_TIME_MS` | `5000U` | 首次识别到疑似边缘后保持 STOP 的非阻塞等待时间；等待期间 YOLO、IPC 和 CPU1 的 3/6 破损仲裁继续运行，期满后未被锁定才允许脱险转向。 | B/C |
| `NAV_ESCAPE_PRETURN_STOP_FRAMES` | `1U` | 开始转向前要求的 STOP 帧数。 | B/C |
| `NAV_ESCAPE_MIN_TURN_TIME_MS` | `250U` | 每次脱险至少转动时间。 | B |
| `NAV_ESCAPE_MAX_TURN_TIME_MS` | `1350U` | 100% PWM 下单方向最多约转动 180 度；获得可信安全判断时仍可提前停止。 | B |
| `NAV_ESCAPE_MOVING_SAFE_FRAMES` | `2U` | 转动中初步看到安全所需的连续帧数。 | C |
| `NAV_ESCAPE_STOP_SETTLE_TIME_MS` | `120U` | 停轮后等待画面稳定的时间。 | B |
| `NAV_ESCAPE_STOP_SAFE_FRAMES` | `3U` | 停稳后确认可信 FORWARD 所需的连续安全帧数。 | C |
| `NAV_ESCAPE_VERIFY_TIMEOUT_MS` | `600U` | 停稳复核的最长等待时间。 | B/C |
| `NAV_ESCAPE_MAX_DIRECTION_ATTEMPTS` | `2U` | 最多尝试初始方向和反方向，当前编译检查要求必须为 2。 | C |
| `NAV_ESCAPE_RANDOM_SEED` | `0x6D2B79F5` | 初始左右方向伪随机状态的固定种子。 | A |

摄像头位于车体前缘前方，因此“画面刚变安全”不代表车体四角已经安全。当前流程会先停轮、等待画面稳定，再用多帧确认；不能删除这一步。

首次防跌落 STOP 后还会保持停车 `NAV_ESCAPE_YOLO_WAIT_TIME_MS`，为 PhysicalDamage YOLO 的 3/6 投票留出时间。该等待使用 RTOS tick 状态机实现，不阻塞 Navigation Thread；第二方向脱险尝试不会重复等待。

### 3.3 TCM 与 RTT 调试

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `NAV_MODEL_TCM_ENABLE` | `1U` | 把热点代码、工作区和常量放入 ITCM/DTCM 对应段。 | E |
| `NAV_DECISION_RTT_ENABLE` | `0U` | 输出模型原始判断、最终 IPC 动作、分数和门限。 | A |
| `NAV_DECISION_RTT_INTERVAL_FRAMES` | `10U` | 上述日志的帧间隔，仅开日志后生效。 | A |
| `NAV_BINARY_RTT_DUMP_ENABLE` | `0U` | 输出 200x24 二值 ROI；默认关闭以避免运行开销。 | A |
| `NAV_BINARY_RTT_ROWS_PER_FRAME` | `2U` | 每帧输出的二值 ROI 行数。 | A |

## 4. CPU1 电机、安全状态和调试

文件：`Renesas_Cup_Vehicle_CPU1/src/vehicle_thread_entry.c`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `VEHICLE_CONTROL_PERIOD_MS` | `10U` | Vehicle 线程控制周期。 | B |
| `VEHICLE_NAV_COMMAND_TIMEOUT_MS` | `500U` | CPU0 导航命令失联后的停车时间。 | B/C |
| `VEHICLE_STARTUP_SUCTION_PERCENT` | `80U` | 启动后吸附风扇占空比。 | B/E |
| `VEHICLE_NAV_TURN_90_TIME_MS` | `675U` | 当前 100% PWM 下约 90 度原地转向的等效标定值。 | B |
| `VEHICLE_NAV_TURN_CALIBRATION_PERCENT` | `100U` | 上述转向标定使用的 PWM 百分比。 | B |
| `VEHICLE_NAV_TURN_360_EFFORT_LIMIT` | 计算值 `270000 %-ms` | 无 IMU 时按“PWM百分比 x 时间”估算连续单向一整圈，超限后 STOP 并锁定 MANUAL。 | B |
| `VEHICLE_NAV_MOTOR_OUTPUT_ENABLE` | `1U` | CPU0 导航 IPC 电机输出总开关；置 0 可阻止导航命令驱动车轮。 | A/B |
| `VEHICLE_WHEEL_STRAIGHT_TEST_ENABLE` | `0U` | 架空车轮直行自检开关。 | B |
| `VEHICLE_WHEEL_TEST_START_DELAY_MS` | `2500U` | 自检开始前等待时间。 | B |
| `VEHICLE_WHEEL_TEST_RUN_TIME_MS` | `3000U` | 自检运行时间。 | B |
| `VEHICLE_WHEEL_TEST_SPEED_PERCENT` | `90U` | 自检 PWM 百分比。 | B |

文件：`Renesas_Cup_Vehicle_CPU1/src/ipc_thread_entry.c`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `NAV_FORWARD_SPEED_PERCENT` | `90U` | CPU0 请求 FORWARD 时的开环 PWM。 | B |
| `NAV_LEFT_TURN_SPEED_PERCENT` | `100U` | CPU0 请求 TURN_LEFT 时的原地转向 PWM。 | B |
| `NAV_RIGHT_TURN_SPEED_PERCENT` | `100U` | CPU0 请求 TURN_RIGHT 时的原地转向 PWM。 | B |
| `NAV_IPC_ACTION_RTT_ENABLE` | `0U` | 打印 CPU0 请求动作和 CPU1 实际下发动作，默认编译期关闭。 | A |
| `YOLO_SAFETY_CPU1_QUEUE_LENGTH` | `8U` | CPU1 接收 YOLO 结果的软件队列长度。 | B/D |

文件：`Renesas_Cup_Vehicle_CPU1/src/Vehicle/application/vehicle_service.h`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `VEHICLE_MPU6050_ENABLE` | `0U` | MPU6050 功能总开关，当前故障隔离。 | B/E |

文件：`Renesas_Cup_Vehicle_CPU1/src/Vehicle/application/vehicle_service.c`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `VEHICLE_SUCTION_STARTUP_TIME_MS` | `2000U` | 吸附启动阶段持续时间。 | B/E |
| `VEHICLE_AUTO_STRAIGHT_TIME_MS` | `5000U` | 内置 AUTO 轨迹的直行阶段时长。 | B |
| `VEHICLE_AUTO_STRAIGHT_PERCENT` | `90U` | 内置 AUTO 直行 PWM。 | B |
| `VEHICLE_AUTO_TURN_PERCENT` | `90U` | 内置 AUTO 转向 PWM。 | B |
| `VEHICLE_AUTO_RIGHT_TURN_TIME_MS` | `750U` | 内置 AUTO 右转时长。 | B |
| `VEHICLE_MPU6050_ADDRESS` | `0x68U` | MPU6050 I2C 地址。 | E |
| `VEHICLE_GYRO_CALIBRATION_SAMPLES` | `500U` | 陀螺仪零偏标定样本数。 | B/E |
| `VEHICLE_GYRO_CALIBRATION_INTERVAL_MS` | `2U` | 陀螺仪标定采样间隔。 | B/E |

`VEHICLE_NAV_TURN_90_TIME_MS` 当前为 675 ms@100%，`VEHICLE_AUTO_RIGHT_TURN_TIME_MS` 仍为 750 ms@90%，两者服务于不同路径。调导航脱险角度时改前者；调内置 AUTO 轨迹时才改后者。

## 5. Gray8 视频、JPEG 与显示

文件：`Renesas_Cup_Vehicle_CPU0/src/encode_thread_entry.c`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `VIDEO_SOURCE_WIDTH` / `HEIGHT` | `1024U` / `600U` | VIN RGB565 源帧尺寸。 | E |
| `VIDEO_ENCODE_WIDTH` / `HEIGHT` | `200U` / `112U` | 灰度 JPEG/导航数据尺寸。 | C/D/E |
| `VIDEO_ENCODE_QUALITY` | `35U` | Gray8 JPEG 质量。 | A/B |
| `VIDEO_TARGET_PERIOD_MS` | `100U` | 编码目标周期，当前约 10 fps。 | B |

文件：`Renesas_Cup_Vehicle_CPU0/src/ImageUpload/Image_JPEG_Encoder.c`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `IMAGE_UPLOAD_WIDTH` / `HEIGHT` | `240U` / `240U` | 检测事件 JPEG 的图像尺寸。 | D/E |
| `IMAGE_JPEG_MAX_SIZE` | `64U * 1024U` | 单张事件 JPEG 最大缓冲容量。 | D/E |
| `IMAGE_JPEG_WORKER_STACK_BYTES` | `0x2000U` | JPEG worker 栈大小。 | E |
| `IMAGE_JPEG_WORKER_PRIORITY` | `0U` | JPEG worker 优先级参数。 | E |
| `IMAGE_JPEG_QUALITY_MIN` / `MAX` | `1U` / `100U` | JPEG 质量合法范围。 | A |

文件：`Renesas_Cup_Vehicle_CPU0/src/display_thread_entry.c`

| 宏组 | 当前值 | 作用 | 风险 |
|---|---|---|---|
| `DISPLAY_SCALE_SOURCE_WIDTH/HEIGHT` | `1024U / 600U` | LCD 缩放源尺寸。 | E |
| `DISPLAY_SCALE_CROP_X/Y/WIDTH/HEIGHT` | `12U / 0U / 1000U / 600U` | LCD 显示先裁剪的区域，这正是画面看起来被轻微裁剪放大的入口。 | A/E |
| `DISPLAY_SCALE_DESTINATION_WIDTH/HEIGHT` | `800U / 480U` | LCD 目标尺寸。 | E |
| `OVERLAY_WIDTH/HEIGHT` | `800 / 480` | AI 叠加层尺寸。 | A/E |
| `OVERLAY_AI_REGION_X/Y` | `160.0f / 0.0f` | 检测框叠加区域起点。 | A |
| `OVERLAY_MODEL_TO_SCREEN_X/Y` | `3.75f / 3.75f` | 128 模型坐标到显示区域的比例。 | A/C |
| `OVERLAY_BOX_COLOR/LINE_WIDTH` | `0xF800U / 4` | 检测框颜色和线宽。 | A |
| `OVERLAY_TEXT_*` | 见文件顶部 | 标签缩放、颜色、边距和字形布局。 | A |

## 6. 摄像头参数

文件：`Renesas_Cup_Vehicle_CPU0/src/Camera/camera_sensor.h`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `RESET_VALUE` | `0U` | 摄像头复位控制值。 | E |
| `AWB_AUTO_ENABLE` | `0U` | 自动白平衡开关。 | C/E |
| `FPS_TARGET` | `60U` | 传感器目标帧率。 | E |

VIN 输入尺寸、MIPI-CSI lane/时钟和缓冲配置主要位于 FSP 生成配置与摄像头寄存器表中，不属于可独立修改的普通宏。修改分辨率或帧率时必须同时核对 OV5640 输出、MIPI-CSI、VIN、帧缓冲、预处理和显示尺寸，不能只改单个宏。

## 7. IPC 与共享内存协议

以下宏均属于 D 级协议常量。CPU0 与 CPU1 的同名头文件必须保持字节级一致，禁止单边修改。

文件：

- `Renesas_Cup_Vehicle_CPU0/src/IPC/navigation_ipc_protocol.h`
- `Renesas_Cup_Vehicle_CPU1/src/IPC/navigation_ipc_protocol.h`

| 宏 | 当前值 | 作用 |
|---|---:|---|
| `NAV_IPC_MAGIC` | `0xA7U` | 导航动作包标识。 |
| `NAV_IPC_CHECK_XOR` | `0x5AU` | 导航动作包校验混淆常量。 |
| `NAV_IPC_CONTROL_MAGIC` | `0xA8U` | CPU1 反馈控制状态包标识。 |
| `NAV_IPC_CONTROL_CHECK_XOR` | `0xC3U` | 控制状态包校验混淆常量。 |

文件：

- `Renesas_Cup_Vehicle_CPU0/src/IPC/yolo_safety_ipc_protocol.h`
- `Renesas_Cup_Vehicle_CPU1/src/IPC/yolo_safety_ipc_protocol.h`

| 宏 | 当前值 | 作用 |
|---|---:|---|
| `YOLO_SAFETY_IPC_MAGIC` | `0xA9U` | YOLO 安全结果包标识。 |
| `YOLO_SAFETY_IPC_CHECK_XOR` | `0xD6U` | YOLO 安全包校验混淆常量。 |
| `YOLO_SAFETY_IPC_DETECTED_MASK` | `0x80U` | detected 位掩码。 |

YOLO 安全短消息现在只携带源帧序号和阳性标志，不计算、不编码、也不使用源帧年龄或传输延迟。

CPU0 文件 `Renesas_Cup_Vehicle_CPU0/src/ipc_thread_entry.c` 还包含：

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `YOLO_SAFETY_CPU0_QUEUE_LENGTH` | `8U` | CPU0 待发送安全结果队列长度。 | B/D |
| `YOLO_SAFETY_IPC_RETRY_MS` | `5U` | IPC 发送忙时的重试间隔。 | B |

共享 JPEG/视频协议头 `src/IPC/shared_jpeg_protocol.h` 中的 `SHARED_JPEG_*` 和 `SHARED_VIDEO_*` 定义共享内存地址、容量、magic、版本、双槽数量及 IPC 事件值。它们同时存在于双核工程中，属于内存布局和双端协议，不应作为日常调参项。

## 8. NRF 无线参数

文件：`Renesas_Cup_Vehicle_CPU1/src/Radio/application/command_radio.c`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `COMMAND_RADIO_CHANNEL` | `76U` | 遥控命令 nRF24 频道，遥控端必须同步。 | D |
| `COMMAND_RADIO_MAX_FIFO_PACKETS` | `3U` | 单次最多读取的 FIFO 包数量。 | B |

文件：`Renesas_Cup_Vehicle_CPU1/src/Radio/application/video_radio.c`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `VIDEO_RADIO_CHANNEL` | `100U` | 视频 nRF24 频道，接收端必须同步。 | D |
| `VIDEO_RADIO_TIMEOUT_MS` | `50U` | 单次发送超时。 | B |
| `VIDEO_AUTO_ACK_ENABLE` | `0U` | 视频自动应答开关。 | B/D |
| `VIDEO_FAST_BATCH_NO_ACK_ENABLE` | `0U` | 无 ACK 快速批处理开关。 | B/D |
| `VIDEO_NO_ACK_DATA_REPEAT` | `2U` | 普通数据包无 ACK 重发次数。 | B |
| `VIDEO_NO_ACK_BOUNDARY_REPEAT` | `3U` | 帧边界包无 ACK 重发次数。 | B |
| `VIDEO_NO_ACK_PACING_CHUNKS` | `4U` | 无 ACK 发送节拍分块数。 | B |
| `VIDEO_BATCH_SIZE` | `3U` | 快速批处理大小，仅对应开关启用后编译。 | B |
| `VIDEO_BATCHES_BEFORE_DELAY` | `8U` | 插入节拍延迟前的批次数。 | B |
| `VIDEO_EXPECTED_WIDTH/HEIGHT` | `200U / 112U` | 接受的视频尺寸。 | D |

文件：`Renesas_Cup_Vehicle_CPU1/src/Radio/protocol/control_protocol.h` 和 `video_protocol.h` 中的 `*_MAGIC`、`*_VERSION`、包类型、包尺寸、数据偏移和校验索引均为遥控两端协议常量，禁止只改单端。

## 9. Wi-Fi 上传参数

文件：`Renesas_Cup_Vehicle_CPU1/src/wifi_upload_thread_entry.c`

| 宏组 | 作用 | 风险 |
|---|---|---|
| `WIFI_UPLOAD_POWER_STABLE_MS` | DA16200 上电稳定等待。 | B |
| `WIFI_UPLOAD_AT_TIMEOUT_MS` | AT 命令响应超时。 | B |
| `WIFI_UPLOAD_RETRY_DELAY_MS` | 前端连接失败重试间隔。 | A/B |
| `WIFI_UPLOAD_SSID` / `PASSWORD` | Wi-Fi 凭据；具体值不在本文复制。 | 敏感配置 |
| `WIFI_UPLOAD_CONNECT_TIMEOUT_MS` | 加入 AP 的超时。 | B |
| `WIFI_UPLOAD_SERVER_IP` / `PORT` | 前端服务地址和端口。 | A |
| `WIFI_UPLOAD_PROTOCOL_VERSION` / `HEADER_SIZE` | 上传协议版本和头长度，前端必须同步。 | D |
| `WIFI_UPLOAD_JPEG_CHUNK_SIZE` | 单次发送的 JPEG 分块大小。 | B |
| `WIFI_UPLOAD_SEND_TIMEOUT_MS` | 分块发送超时。 | B |
| `WIFI_UPLOAD_FRONTEND_MAX_JPEG_SIZE` | 前端允许的最大 JPEG 长度。 | D |

文件：`Renesas_Cup_Vehicle_CPU1/src/freertos_hooks.c`

| 宏 | 当前值 | 作用 | 风险 |
|---|---:|---|---|
| `APP_STARTUP_WIFI_DEADLINE_MS` | `30000U` | Wi-Fi 未就绪时释放 M33 业务任务的最长等待时间。 | B |

## 10. 推荐调参顺序

1. 调试 YOLO 是否控制电机时，只改 `VEHICLE_YOLO_SAFETY_ENABLE`，不要同时改 60% 和 3/6。
2. 调 60% 时改 CPU0 的 `AI_SAFETY_CONFIDENCE_THRESHOLD`，先保留 CPU1 的 3/6 不动，再做录制数据回放和实车低速验证。
3. 调脱险转角时先架空车轮标定 `VEHICLE_NAV_TURN_90_TIME_MS` 与 PWM，再在受控平台调 `NAV_ESCAPE_MAX_TURN_TIME_MS`。
4. 调黑白模型门限、灰度阈值、ROI 或确认帧时，必须重跑 311 张离线定点回归和按 `source_file + segment` 的分组验证。
5. 修改 `*_MAGIC`、共享内存地址、包长度、视频尺寸或无线频道时，先列出所有通信端并一次同步修改。

## 11. 编译与验证清单

每次宏修改后至少完成以下检查：

```text
[ ] CPU0 完整编译通过
[ ] CPU1 完整编译通过
[ ] 未生成或修改 configuration.xml、ra_gen 和上电初始化代码
[ ] 双核协议头保持一致
[ ] RTT 中无新的 IPC 校验、队列溢出或超时错误
[ ] 涉及电机时先架空车轮，再低速落地验证
[ ] 涉及防跌落或 YOLO 时完成离线回放和实车 STOP/MANUAL 锁定验证
```

关闭某个安全宏只应用于定位故障。正式运行前应恢复安全开关，并确认 AUTO 重新授权、STOP 优先级和 MANUAL 锁定行为符合预期。
