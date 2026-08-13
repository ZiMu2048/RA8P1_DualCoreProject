# CPU0 彩色 JPEG 合并检查点

## 范围

本检查点对应移植计划的阶段 0 与阶段 1，修改范围只包含 CPU0 彩色 JPEG 模块、AI Thread 接口和本地 Debug 构建清单喵。

未修改 Encode Thread、IPC、CPU1、TCM、`configuration.xml`、`ra_gen` 业务代码和 FSP 外设配置喵。

## 基线冻结

- `preexisting_tracked_changes.patch` 保存开始工作前的全部已跟踪改动喵。
- `prebuild_artifacts.zip` 保存开始 Clean Build 前的 ELF、MAP 和 SREC 喵。
- `clean_baseline_artifacts.zip` 保存修正已有 nRF 构建清单遗漏后的 CPU0 干净基线喵。
- 基线尺寸为 `text=745822`、`data=702`、`bss=8660520` 字节喵。

基线第一次链接失败的原因为 `ra_gen/main.c` 已调用 `nRF27L01_create()`，但已有 `ra_gen/nRF27L01.c` 和 `src/nRF27L01_entry.c` 未进入 Debug 构建清单喵。

该问题只通过刷新本地 Debug 构建清单解决，没有修改 nRF 线程业务代码喵。

## 彩色 JPEG 合并结果

- `src/ImageUpload/Image_JPEG_Encoder.c/.h` 与已验证 M85 参考工程逐文件 SHA-256 一致喵。
- `src/ThirdParty/stb_image_write.h` 与已验证 M85 参考工程逐文件 SHA-256 一致喵。
- AI Thread 使用 `1024x600` RGB565 源帧，居中裁剪 `600x600`，缩放为 `240x240` RGB888，并以质量 `60` 编码喵。
- JPEG Worker 使用静态 `0x2000` 字节栈和 FreeRTOS 优先级 `0` 喵。
- 一次检测事件只排队一张彩色 JPEG，连续 10 个正常推理帧后重新武装喵。
- `color_jpeg_merge_artifacts.zip` 保存合并后的 ELF、MAP 和 SREC 喵。

最终 Clean Build 成功，尺寸为 `text=757030`、`data=706`、`bss=8907176` 字节喵。

相对干净基线增量为 `text=+11208`、`data=+4`、`bss=+246656` 字节喵。

MAP 验证结果如下喵。

| 对象 | 地址 | 大小 |
|---|---:|---:|
| `g_upload_rgb888` | `0x68000900` | `0x2A300` |
| `g_upload_jpeg` | `0x6802AC00` | `0x10000` |
| `g_image_jpeg_worker_stack` | `0x220E4700` | `0x2000` |
| `g_image_jpeg_worker_tcb` | `0x220E6700` | `0x68` |
| `g_upload_jpeg_size` | `0x220E6774` | `0x4` |
| SDRAM 已用段结束 | `0x6878DC00` | - |
| SHAREMEM | `0x6FFE0000` | `0x20000` |

彩色 JPEG 私有缓冲区位于普通 SDRAM，未进入 SHAREMEM，也未与当前帧缓冲区或显示缓冲区重叠喵。

## 首个板上 RTT 调试点

使用 CPU0 主调试配置烧录并复位运行，首先应看到以下日志喵。

```text
[JPEG] Worker ready: 240x240 quality=60 rearm=10.
```

第一次制造模型检测事件后应只出现一组以下日志，其中两行 `frame` 必须相同，`size` 必须大于 4 且不超过 65536 喵。

```text
[JPEG] Queued frame=<N>.
[JPEG] Encoded frame=<N> size=<S>.
```

成功日志只会在编码器已经验证 JPEG 的 `FF D8` 起始标记和 `FF D9` 结束标记后出现喵。

如需调试器直接复核，可在 `0x6802AC00` 观察 `FF D8`，读取 `0x220E6774` 的有效长度 `S`，再在 `0x6802AC00 + S - 2` 观察 `FF D9` 喵。

持续保持检测事件时不得重复出现 `Queued`，随后让模型连续输出 10 个正常帧，应看到以下重新武装日志喵。

```text
[JPEG] Trigger rearmed after 10 clean frames.
```

再次制造检测事件后应出现一组新序号的 `Queued` 与 `Encoded`，同时 Camera、LCD、AI 推理和检测框必须继续运行喵。

出现任意 `[JPEG][ERR]`、Camera/Display/AI 停止、HardFault、栈溢出或 JPEG 长度越界时均判定阶段 1 未通过喵。
