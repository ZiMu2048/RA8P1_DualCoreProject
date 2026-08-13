# RA8P1 裸机单核到 FreeRTOS 双核移植计划书

## 1. 文档信息

| 项目 | 内容 |
|---|---|
| 目标平台 | Renesas RA8P1，Cortex-M85 + Cortex-M33 + Ethos-U55 |
| 目标系统 | CPU0、CPU1 均运行 FreeRTOS |
| 当前工程根目录 | `D:\Lab\Lab_MCU\Renesas_RA\RA8P1_DualCoreProject` |
| 单核业务基线 | `D:\Lab\Lab_MCU\Renesas_RA\RA8P1_CAM_GLCDC_AI\CameraTestProject\e2studio` |
| 已验证 M85 参考工程 | `D:\Lab\Lab_MCU\Renesas_RA\RA8P1DualCoreProject_CPU0\RA8P1DualCoreProject_CPU0` |
| 编写日期 | 2026-08-13 |
| 当前阶段 | 阶段 4 已完成硬件验收，下一阶段为 CPU1 DA16200 上传 |

本计划书用于指导后续代码合并、构建、硬件调试和新任务交接，后续工作以本文件记录的职责边界与实施顺序为准喵。

## 2. 已冻结的架构决策

### 2.1 彩色 JPEG 与黑白 JPEG 完全分离

以前工程中已经验证的彩色 JPEG 编码实现保持原结构，不把它改造成 Encode Thread 的业务，也不随意改写其内部缓冲区、Helium 转换、编码接口和异常触发去抖逻辑喵。

彩色 JPEG 用于 AI 判断出错误图像后，通过共享 SDRAM 和 IPC 交给 M33，再由 DA16200 上传网页端喵。

`Encode Thread` 只负责黑白 JPEG 编码，服务于后续 nRF24L01 实时黑白图传，不负责彩色错误图像编码喵。

黑白 JPEG 可以复用 Helium 和第三方 JPEG 编码库，但必须使用独立接口、独立工作缓冲区和独立输出缓冲区，不与彩色 JPEG 共用可被同时修改的编码状态喵。

黑白 JPEG 与 nRF24L01 图传属于后续阶段，当前先不实现喵。

### 2.2 nRF24L01 保留在 Cortex-M33

nRF24L01 的 SPI 驱动和 `Video TX Thread` 保留在 CPU1/M33，不迁移到 CPU0/M85，从而避免移动 SPI 外设、引脚和线程 Stack 归属喵。

后续黑白图传的数据路径如下喵。

```text
M85 Encode Thread
    │ 生成黑白 JPEG
    ▼
共享 SDRAM / IPC 描述符
    ▼
M33 IPC Thread
    │ 投递发送作业
    ▼
M33 Video TX Thread
    │ SPI
    ▼
nRF24L01
```

M85 不直接操作 nRF24L01，M33 的 Video TX Thread 是 nRF24L01 图像发送硬件的唯一所有者喵。

### 2.3 彩色错误图像 IPC 方案固定

CPU0 把已经完成的彩色 JPEG 复制到共享 SDRAM，完成长度、序号和 CRC 填写后执行 D-cache clean 与内存屏障，再通过 IPC 发送短通知喵。

CPU1 收到通知后只读取共享 JPEG，完成协议校验和 CRC 校验，再交给 Wi-Fi Upload Thread 通过 DA16200 上传喵。

CPU1 上传完成或失败后必须写入完成状态和错误码，并通过 IPC 回执 CPU0，CPU0 收到回执后才能重新使用共享载荷区喵。

### 2.4 M33 电机业务独立调度

电机、灯光、IMU 和安全控制由 M33 的 Vehicle Thread 负责，网络上传和图传不能直接调用电机驱动，也不能长期持有电机业务使用的互斥锁或临界区喵。

DA16200 的阻塞式 AT、Wi-Fi 和 TCP 操作只能阻塞 Wi-Fi Upload Thread，不能阻塞 Vehicle Thread、Command RX Thread 或整个 Cortex-M33 喵。

### 2.5 TCM 延后处理

当前阶段不迁移任务栈、模型、图像缓冲区或热点函数到 ITCM、DTCM、CTCM 和 STCM 喵。

先保证 Camera、Display、AI、JPEG、IPC、DA16200、电机和 nRF24L01 的完整功能落地，再根据 MAP、运行时间和栈水位数据决定是否使用 TCM 喵。

## 3. 最终双核职责

```text
Cortex-M85 / CPU0
├── IPC Thread
│   └── IPC初始化、门铃、回执和共享状态推进
├── Display Thread
│   └── GLCDC背景层、D/AVE 2D图层二、VSYNC和换帧
├── Camera Thread
│   └── IIC、OV5640、MIPI-CSI、VIN和三缓冲采集
├── AI Thread
│   └── Helium预处理、Ethos-U55、YOLO后处理和错误事件判断
├── Encode Thread
│   └── 仅负责黑白JPEG，供后续nRF24L01图传
└── 彩色JPEG Worker
    └── 保留以前工程已经验证的彩色错误图像编码方式

Cortex-M33 / CPU1
├── Vehicle Thread
│   └── 电机、灯光、IMU、控制器和安全停止
├── Command RX Thread
│   └── 遥控命令接收，只向Vehicle mailbox提交命令
├── IPC Thread
│   └── 接收M85描述符、校验共享数据并分发业务作业
├── Wi-Fi Upload Thread
│   └── DA16200、AT、Wi-Fi、TCP和网页端上传
└── Video TX Thread
    └── SPI、nRF24L01和实时黑白图像发送
```

## 4. M85 线程优先级与运行规则

FreeRTOS 中数字越大优先级越高，CPU0 当前配置先保持不变喵。

| 调度等级 | 线程 | 优先级 | 栈大小 | 正式职责 | 等待方式 |
|---:|---|---:|---:|---|---|
| 1 | IPC Thread | 7 | 1024 B | IPC门铃、回执、共享状态 | 任务通知或事件，永久阻塞 |
| 2 | Display Thread | 6 | `0x1000` | GLCDC、VSYNC、背景层和图层二 | Camera帧事件与VSYNC事件 |
| 3 | Camera Thread | 4 | `0x1800` | 摄像头初始化、VIN启动和错误处理 | 初始化屏障与错误事件 |
| 4 | AI Thread | 3 | `0x2000` | 预处理、NPU推理、YOLO和异常判定 | 独立AI输入事件 |
| 5 | Encode Thread | 2 | 1024 B | 仅黑白JPEG编码 | 静态队列或任务通知 |
| 6 | 彩色JPEG Worker | 0 | `0x2000` | 保留已验证彩色JPEG编码 | 模块内部任务通知 |

### 4.1 CPU0 IPC Thread 保持优先级 7 的条件

CPU0 IPC Thread 可以保持当前最高优先级 7，因为它需要及时接收 M33 的 READY、DONE 和 ERROR 回执，避免共享缓冲区长期保持占用状态喵。

该线程必须满足以下硬性限制喵。

1. IPC Thread 中不执行 JPEG 编码、图像缩放、整帧复制和 CRC 全量计算喵。
2. IPC Thread 中不执行 DA16200、nRF24L01 或任何外设的阻塞式发送喵。
3. IPC Thread 每次被唤醒只读取短消息、记录状态、唤醒业务线程，然后立即重新阻塞喵。
4. ISR 中只保存消息、设置标志并使用 FromISR API 唤醒 IPC Thread，不打印大量 RTT 日志喵。
5. IPC Thread 不使用 `vTaskDelay(1)` 周期轮询，正式版必须在没有消息时完全阻塞喵。
6. 单次 IPC Thread 运行时间应使用 GPIO 或周期计数器测量，目标控制在 50 微秒以内喵。

若后续测量发现 IPC Thread 连续唤醒导致 Display Thread 丢失 VSYNC 或 Camera 帧处理抖动，应先限制 IPC 消息频率和单次处理数量，而不是直接提高其他计算线程的优先级喵。

### 4.2 CPU0 初始化与调度顺序

FSP 的线程创建顺序不是硬件初始化顺序，真正执行顺序由线程优先级、初始化屏障和阻塞点共同决定喵。

推荐运行顺序如下喵。

```text
IPC Thread初始化IPC后阻塞
        ↓
Display Thread初始化GLCDC与D/AVE 2D并发布DISPLAY_INIT_DONE
        ↓
Camera Thread初始化OV5640，等待Display就绪后启动VIN
        ↓
AI Thread初始化Ethos-U55，等待Camera帧
        ↓
Encode Thread等待后续黑白JPEG作业
        ↓
彩色JPEG Worker等待AI提交彩色快照
```

## 5. M33 线程优先级与业务隔离

CPU1 当前线程优先级先保持如下配置喵。

| 调度等级 | 线程 | 优先级 | 栈大小 | 正式职责 |
|---:|---|---:|---:|---|
| 1 | Vehicle Thread | 7 | 1024 B | 10 ms电机、灯光、IMU和安全控制周期 |
| 2 | Command RX Thread | 6 | 1024 B | nRF遥控或其他命令接收与mailbox提交 |
| 3 | IPC Thread | 5 | 1024 B | IPC接收、共享数据校验和业务分发 |
| 4 | Wi-Fi Upload Thread | 4 | 1024 B | DA16200阻塞式上传 |
| 5 | Video TX Thread | 2 | 1024 B | nRF24L01黑白图传 |

M33 IPC Thread 的优先级 5 低于 Vehicle Thread 和 Command RX Thread，因此高频图像 IPC 不能打断电机控制周期和遥控命令接收喵。

M33 IPC Thread 的优先级又高于 Wi-Fi Upload Thread 和 Video TX Thread，因此它可以及时释放 IPC 门铃并把作业投递到对应业务线程喵。

IPC Thread 只负责校验描述符和投递作业，DA16200 的 TCP 分块发送由 Wi-Fi Upload Thread 完成，nRF24L01 的 SPI 分包发送由 Video TX Thread 完成喵。

Vehicle Thread、Command RX Thread、IPC Thread、Wi-Fi Upload Thread 和 Video TX Thread 之间不得通过长临界区保护大块图像数据喵。

## 6. 数据所有权与 SDRAM 规则

### 6.1 VIN 与 AI/Display

VIN 是三个原始 RGB565 缓冲区的硬件生产者，Camera ISR 只发布最新完成缓冲区指针和序号喵。

Display Thread 和 AI Thread 在读取 VIN 完成帧前必须执行 D-cache invalidate，且不能在处理完成后继续保存原始 VIN 指针喵。

AI Thread 在调用 NPU 前只需要把模型输入和必要的彩色 JPEG 快照复制到各自私有缓冲区，完成复制后立即释放对 VIN 帧的逻辑所有权喵。

### 6.2 彩色 JPEG

彩色 JPEG 使用以前工程已经验证的私有 RGB888 快照和 JPEG 输出缓冲区喵。

AI Thread 负责异常触发、快照生成和作业提交，彩色 JPEG Worker 负责压缩，IPC 发布方只能读取已经完成的 JPEG 数据喵。

下一次彩色编码开始前，IPC 发布方必须已经把上一份 JPEG 完整复制到共享 SDRAM，避免编码模块覆盖仍在使用的数据喵。

### 6.3 黑白 JPEG

黑白 JPEG 使用 Encode Thread 独占的灰度快照、JPEG 输出缓冲区和编码上下文喵。

在黑白图传阶段开始前，需要先确定灰度分辨率、JPEG质量、目标帧率、单帧最大字节数和 nRF24L01 分包协议喵。

彩色错误图像和实时黑白图像不得同时写同一个共享载荷区，后续应根据实际带宽选择分区、双缓冲或描述符环形队列喵。

### 6.4 当前共享 SDRAM

当前 Solution 已预留以下共享窗口喵。

| 区域 | 地址 | 大小 |
|---|---:|---:|
| SHAREMEM | `0x6FFE0000` | `0x00020000` |

参考 M85 工程的共享 JPEG 模块使用 `0x6FFC0000` 和 `0x00040000`，与当前工程不一致，后续只能复用协议思想和状态机，不能直接复制硬编码地址喵。

共享控制块与载荷必须限制在 `0x6FFE0000～0x6FFFFFFF` 范围内，并通过编译期断言和 MAP 检查确认没有越界喵。

## 7. D-cache 与内存屏障清单

| 数据流 | CPU0操作 | CPU1操作 |
|---|---|---|
| VIN写帧，M85读取 | 读取前invalidate | 不涉及 |
| M85写GLCDC背景层 | 写完clean，随后`__DMB()` | 不涉及 |
| D/AVE 2D写图层二 | 保持现有已验证缓存顺序 | 不涉及 |
| M85写共享JPEG | payload clean、control clean、`__DMB()` | D-cache关闭，读取前执行屏障 |
| M33更新共享控制块 | M85读取前invalidate和`__DMB()` | 写状态后执行`__DMB()` |
| M85写黑白图传缓冲 | 写完clean和`__DMB()` | 读取前检查状态、长度和序号 |

Cache clean 和 invalidate 的地址与长度必须覆盖完整有效区，并满足 CMSIS 接口要求的对齐规则喵。

`volatile` 只能防止编译器省略控制块访问，不能替代 D-cache 维护、原子操作和内存屏障喵。

## 8. 文件迁移边界

### 8.1 当前 CPU0 已有并优先保留

- `src/Camera/`
- `src/Display/`
- `src/Helium/`
- `src/AI/`
- `src/model/`
- `src/SEGGER_RTT/`
- `src/camera_thread_entry.c`
- `src/display_thread_entry.c`
- `src/ai_thread_entry.c`

这些模块不从旧工程整体覆盖，只在明确的当前阶段修改必要接口喵。

### 8.2 彩色 JPEG 阶段迁移

- `src/ImageUpload/Image_JPEG_Encoder.c`
- `src/ImageUpload/Image_JPEG_Encoder.h`
- `src/ThirdParty/stb_image_write.h`
- 参考工程中 AI Thread 对彩色 JPEG 的快照、提交和异常去抖接入逻辑喵。

彩色 JPEG 模块按以前工程的已验证结构迁移，不合并进 Encode Thread 喵。

### 8.3 黑白 JPEG 阶段新增

- 独立黑白图像转换模块喵。
- 独立黑白 JPEG 编码接口喵。
- Encode Thread 的静态作业队列与状态统计喵。
- 独立灰度快照和 JPEG 输出缓冲区喵。

该阶段开始前先冻结彩色 JPEG 基线，防止黑白编码修改影响彩色错误图像上传喵。

### 8.4 IPC 阶段迁移并适配

- `src/IPC/shared_jpeg_protocol.c/.h`
- `src/IPC/shared_jpeg_cpu0.c/.h`
- CPU1 对应的共享数据消费者模块喵。

必须把共享基址、容量、状态机和消息编号改为当前 Solution 的实际规划，并重新验证双方结构体大小和字段偏移喵。

### 8.5 CPU1 网络阶段迁移

- 单核工程的 `src/DA16200/`
- 单核工程的 `src/RingBuffer/`
- 图像 TCP 头、分块发送和错误恢复逻辑喵。

DA16200 模块只允许 Wi-Fi Upload Thread 调用，UART ISR 只负责接收字节、记录错误和通知任务喵。

### 8.6 禁止直接复制

- 旧工程和参考工程的 `ra/` 喵。
- 旧工程和参考工程的 `ra_gen/` 喵。
- 旧版本 `configuration.xml` 喵。
- 旧链接脚本和旧内存区域文件喵。
- 参考工程中含 Git 冲突标记的 `ipc_thread_entry.c` 和生成的 `encode_thread.c` 喵。

## 9. 分阶段实施与验收计划

### 阶段 0：冻结当前 CPU0 基线

工作内容如下喵。

1. Clean Build 当前 CPU0 工程并保存 MAP、ELF、SREC 和构建日志喵。
2. 由用户验证 Camera、Display、AI 和 D/AVE 2D 图层二画框喵。
3. 记录 VIN 帧计数、GLCDC换帧计数、AI结果序号和各线程栈水位喵。
4. 建立修改前恢复检查点，不修改 XML 喵。

验收标准为摄像头连续采集、LCD稳定显示、AI持续推理、检测框正常且无新增构建错误喵。

### 阶段 1：合并已验证彩色 JPEG

工作内容如下喵。

1. 迁移 `ImageUpload` 和 `stb_image_write.h` 喵。
2. 保留彩色 JPEG 内部 Worker、私有缓冲区和原异常去抖逻辑喵。
3. 将彩色 JPEG 快照和提交逻辑接入当前 AI Thread 喵。
4. 不修改 Encode Thread，不接入 IPC，不启动 CPU1 网络业务喵。
5. 对新增文件和当前 AI Thread 做构建检查喵。

验收标准如下喵。

- 编码结果以 `FF D8` 开始并以 `FF D9` 结束喵。
- 输出尺寸、质量和最大容量与以前工程一致喵。
- 一次连续错误事件只触发一次彩色 JPEG，连续十帧正常后重新武装喵。
- JPEG Worker 运行期间 Camera、Display 和 AI 不停止喵。
- 用户确认硬件运行结果后才进入下一阶段喵。

### 阶段 2：完成 CPU0 彩色图像业务闭环

工作内容如下喵。

1. 确认 AI 结果发布与 Display Thread 图层二显示不受彩色 JPEG 影响喵。
2. 测量预处理、NPU、后处理、彩色快照和 JPEG 编码时间喵。
3. 检查 JPEG Worker、AI Thread、Display Thread 和 Camera Thread 栈水位喵。
4. 连续运行并检查 VIN 缓冲区是否被错误长期持有喵。

验收标准为 CPU0 摄像头、显示、AI、画框和彩色 JPEG 可以长期共同运行喵。

### 阶段 3：定义共享 JPEG 协议，不接 DA16200

工作内容如下喵。

1. 根据当前 `0x6FFE0000 + 0x20000` 共享区定义控制块和载荷边界喵。
2. 定义 magic、version、state、sequence、length、CRC 和 error 字段喵。
3. 实现 `FREE → M85_FILLING → READY_FOR_M33 → M33_PROCESSING → DONE/ERROR → FREE` 状态机喵。
4. CPU0 IPC Thread 和 CPU1 IPC Thread 先只完成递增计数器测试喵。
5. 再完成固定字符串共享内存与 CRC 测试喵。

验收标准为双核 IPC 序号连续、共享状态可恢复、CRC一致，并且电机控制周期不受影响喵。

### 阶段 4：传输彩色错误 JPEG

工作内容如下喵。

1. CPU0 将完成的彩色 JPEG 复制到共享载荷区喵。
2. CPU0 完成 D-cache clean、`__DMB()` 和 READY 状态发布喵。
3. CPU1 校验描述符、SOI、EOI、长度和 CRC 喵。
4. CPU1 暂不联网，只统计接收成功、失败和超时喵。
5. CPU1 完成后回执 CPU0 并释放共享载荷区喵。

验收标准为同一 JPEG 在 CPU0 私有缓冲区和 CPU1 读取结果中逐字节或 CRC 一致喵。

### 阶段 5：M33 DA16200 网页上传

工作内容如下喵。

1. 把 DA16200 和 RingBuffer 迁入 CPU1 喵。
2. Wi-Fi Upload Thread 独占 UART 和 DA16200 状态机喵。
3. IPC Thread 只提交上传作业，不执行 AT 或 TCP 发送喵。
4. Vehicle Thread 始终以 10 ms 周期独立运行，不等待网络线程喵。
5. 测试网络断开、TCP超时和DA16200复位时的共享缓冲区恢复喵。

验收标准为网页端收到正确 JPEG，同时电机控制周期、遥控命令和安全停止功能保持正常喵。

### 阶段 6：黑白 JPEG 与 nRF24L01 图传

该阶段在彩色错误图像上传和电机业务稳定后再开始喵。

工作内容如下喵。

1. 确定黑白图像分辨率、JPEG质量、目标帧率和无线分包大小喵。
2. 为 Encode Thread 实现独立灰度快照和黑白 JPEG 编码喵。
3. 通过共享 SDRAM 和 IPC 向 M33 Video TX Thread 提交黑白帧喵。
4. Video TX Thread 独占 nRF24L01 SPI1，并实现序号、分包、丢包和超时统计喵。
5. 限制图传 IPC 频率，避免高频门铃干扰 CPU0 Display 和 CPU1 Vehicle Thread 喵。

验收标准为黑白图传持续运行时，彩色错误图像上传、电机控制、AI和LCD显示均不失效喵。

### 阶段 7：可靠性、性能与 TCM 评估

工作内容如下喵。

1. 测量全部线程栈水位、CPU占用、关键函数执行时间和共享缓冲区占用时间喵。
2. 检查 Camera、GLCDC、D/AVE 2D、NPU、JPEG和IPC的全部缓存维护点喵。
3. 进行网络断开、CPU1未启动、IPC丢通知、nRF掉线和摄像头异常测试喵。
4. 功能稳定后再评估把热点代码、只读表或任务栈迁移到 TCM 喵。

TCM 优化必须一次只移动一个对象，并通过 MAP 地址、性能数据和冷启动测试验证喵。

## 10. XML 与生成文件修改规则

初期优先保持现有 `configuration.xml`、`solution.xml` 和 `ra_gen` 不变喵。

新增用户模块、静态缓冲区、任务通知和用户代码不需要修改 XML 时，不为了形式重新生成 FSP 配置喵。

只有出现以下情况时才通过 FSP Configurator 修改 XML 喵。

1. 必须调整现有线程栈大小或优先级喵。
2. 必须新增或修改 FSP 外设实例、IRQ、DMA/DTC 或回调喵。
3. 当前共享内存配置无法满足已经量化的彩色和黑白图像容量喵。
4. 后续 TCM 实验需要新增明确段放置喵。

不得手工编辑 `ra_gen` 生成文件来绕过 FSP 配置喵。

## 11. 调试与协作顺序

每个阶段遵循“助手修改并构建检查，用户烧录并硬件调试，用户返回证据，再继续下一阶段”的顺序喵。

用户每次调试后建议返回以下证据喵。

- RTT完整启动日志喵。
- 首个错误前后的日志喵。
- 关键计数器和缓冲区地址喵。
- 对应线程栈水位喵。
- LCD、网络、nRF或电机的实际现象喵。
- 必要时提供逻辑分析仪、示波器或内存窗口截图喵。

一次只改变一个模块或一个关键变量，上一阶段没有形成可观察闭环前不进入下一阶段喵。

## 12. 2026-08-13 实施结果与硬件验收

### 12.1 已完成内容

当前双核工程已经完成以下实现并通过构建检查喵。

1. CPU0 彩色 JPEG Worker 已接入 AI 异常触发路径，继续使用私有 RGB888 快照和私有 JPEG 输出缓冲区喵。
2. JPEG 异步状态已经限制为只有 `IDLE` 状态才能生成下一份快照，避免编码期间覆盖 Worker 正在读取的 RGB888 数据喵。
3. CPU0 在 `BSP_WARM_START_POST_C` 阶段完成引脚和 SDRAM 初始化后调用 `R_BSP_SecondaryCoreStart()`，CPU1 随正常上电路径自动启动，不再由 IPC 应用协议负责启动喵。
4. CPU0 调试配置已经加入 CPU1 ELF 下载项，因此从 CPU0 调试配置启动时会同时下载两个核心的程序镜像喵。
5. CPU0 和 CPU1 都加入了独立共享 JPEG 模块，IPC Channel 0 只发送 `DATA_READY`、`DONE` 和 `ERROR` 短消息喵。
6. JPEG 字节通过当前共享 SDRAM 窗口传递，CPU0 和 CPU1 分别维护生产端与消费端状态喵。
7. 新增函数已经按照工程原有的 `[@name]`、`[@type]`、`[@usage]`、`[@argument]` 和 `[@return]` 风格补齐注释喵。
8. CPU0 和 CPU1 Debug 工程均已成功生成 ELF 与 SREC，新增共享 JPEG 代码没有编译错误喵。

### 12.2 当前共享 JPEG 协议

| 项目 | 当前值 |
|---|---:|
| 共享窗口 | `0x6FFE0000～0x6FFFFFFF` |
| 窗口大小 | `0x00020000`，即 128 KiB |
| 控制块 | `0x6FFE0000`，64 字节 |
| JPEG 载荷 | 从 `0x6FFE0040` 开始 |
| 最大 JPEG 长度 | `131008` 字节 |
| 协议版本 | `1` |
| CRC | CRC-32/ISO-HDLC，反射多项式 `0xEDB88320` |
| 状态机 | `FREE → M85_FILLING → READY_FOR_M33 → M33_PROCESSING → DONE/ERROR → FREE` |
| CPU1 回执超时 | 5000 ms，超时后 CPU0 释放单槽并报告错误 |

CPU0 发布前会检查 JPEG 长度、`FF D8` 起始标记和 `FF D9` 结束标记，然后复制载荷、计算 CRC、执行缓存 clean 与内存屏障，最后发布 READY 状态和 IPC 门铃喵。

CPU1 会检查 magic、version、控制头长度、载荷偏移、状态、消息类型、载荷长度、SOI、EOI 和 CRC，只有全部一致才写入 DONE 并向 CPU0 回执喵。

CPU1 当前 D-cache 关闭，但消费端仍保留了条件式 invalidate 封装，后续若打开 M33 D-cache，不应删除该缓存维护边界喵。

### 12.3 硬件实测证据

用户在目标板上连续取得以下类型的完整日志闭环喵。

```text
[JPEG] Queued frame=3754.
[JPEG] Encoded frame=3754 size=7410.
[SHM0] Published frame=3754 size=7410.
[SHM0] CPU1 verified frame=3754.
```

后续帧 `3823`、`3869`、`3885` 和 `3898` 也分别得到对应的 `CPU1 verified`，JPEG 长度约为 7.3～7.4 KiB，未出现 CRC 拒绝、协议错误或 CPU1 回执超时喵。

因此以下结论已经由硬件日志确认，不需要在新对话中重复验证喵。

- CPU1 镜像已被正确下载，并由 CPU0 的正常上电路径成功启动喵。
- CPU0 和 CPU1 的 IPC Channel 0 双向短消息可用喵。
- CPU1 可以读取 CPU0 写入当前共享 SDRAM 窗口的完整 JPEG 喵。
- JPEG 控制字段、SOI、EOI 和 CRC32 在两个核心之间一致喵。
- CPU0 在收到 CPU1 回执后能够释放共享单槽并继续处理下一次异常事件喵。
- 连续十帧正常后的 JPEG 触发重新武装逻辑仍然正常喵。

### 12.4 当前关键文件

CPU0 当前关键实现如下喵。

- `RA8P1DualCoreProject_CPU0/src/hal_warmstart.c`：SDRAM 初始化后自动启动 CPU1 喵。
- `RA8P1DualCoreProject_CPU0/src/ImageUpload/Image_JPEG_Encoder.c/.h`：彩色 JPEG 快照、后台编码和结果发布喵。
- `RA8P1DualCoreProject_CPU0/src/ai_thread_entry.c`：AI 异常触发、JPEG 作业提交和共享发布喵。
- `RA8P1DualCoreProject_CPU0/src/IPC/shared_jpeg_protocol.c/.h`：共享布局、状态、消息编号和 CRC32 喵。
- `RA8P1DualCoreProject_CPU0/src/IPC/shared_jpeg_cpu0.c/.h`：CPU0 生产端、缓存维护、门铃重试、回执和超时恢复喵。
- `RA8P1DualCoreProject_CPU0/src/ipc_thread_entry.c`：CPU0 IPC ISR 唤醒和回执日志喵。
- `RA8P1DualCoreProject_CPU0/RA8P1DualCoreProject_CPU0.elf.launch`：同时下载 CPU0 与 CPU1 ELF 喵。

CPU1 当前关键实现如下喵。

- `RA8P1DualCoreProject_CPU1/src/IPC/shared_jpeg_protocol.c/.h`：必须与 CPU0 协议文件保持字段和值一致喵。
- `RA8P1DualCoreProject_CPU1/src/IPC/shared_jpeg_cpu1.c/.h`：CPU1 控制头、JPEG 边界和 CRC 校验以及回执重试喵。
- `RA8P1DualCoreProject_CPU1/src/ipc_thread_entry.c`：CPU1 IPC ISR 唤醒、任务校验和 RTT 结果日志喵。

### 12.5 当前实现边界和后续注意事项

当前只使用一个共享 JPEG 槽位，同一时刻最多存在一张尚未得到 CPU1 回执的 JPEG，结构简单且适合当前低频错误图上传喵。

CPU1 IPC Thread 每 100 ms 进行一次安全状态检查，用于恢复 CPU1 启动交错或门铃丢失场景；正常路径仍由 IPC 中断立即唤醒，不会等待轮询周期喵。

当前 CPU1 只完成 JPEG 验证并回执，尚未把 JPEG 作业转交给 Wi-Fi Upload Thread，也尚未迁移 DA16200 与 RingBuffer 喵。

目前不要把黑白实时图传加入这个彩色 JPEG 单槽，也不要让 DA16200 的阻塞 AT/TCP 操作进入 IPC Thread 喵。

若 FSP Configurator 重新生成工程，应检查 Debug 构建列表仍包含两个核心各自的 `src/IPC` 源文件，并确认 CPU0 launch 仍包含 CPU1 ELF 下载项喵。

## 13. 新对话的下一任务

下一阶段从阶段 5 开始：把已经验证的 DA16200 和 RingBuffer 代码迁入 CPU1，并由 Wi-Fi Upload Thread 独占 UART、AT 状态机和 TCP 上传喵。

推荐验收顺序如下喵。

1. 先让 CPU1 Wi-Fi Upload Thread 输出固定启动日志并保持 Vehicle Thread 正常调度喵。
2. 再完成 DA16200 串口固定 AT 命令与应答闭环，不接共享 JPEG 喵。
3. 再让 IPC Thread 把已经 CRC 验证的 JPEG 描述符投递给 Wi-Fi Upload Thread，IPC Thread 不执行网络发送喵。
4. 最后上传一张共享 JPEG，并在网页端核对文件可解码、尺寸正确且字节长度与 CPU0 日志一致喵。
5. 测试网络断开、TCP 超时和 DA16200 复位时，Vehicle Thread 不被阻塞，并且共享 JPEG 槽能在明确错误回执后恢复喵。

新对话开始时应先阅读本文件第 12 节，不要重新实现 CPU1 启动、IPC 计数器测试或共享 JPEG CRC 闭环喵。
