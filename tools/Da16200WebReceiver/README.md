# DA16200 本地图像传输监视器

双击 `start_receiver.bat` 即可启动。

- DA16200 TCP 目标：`192.168.137.1:5000`
- 本地网页：`http://127.0.0.1:8000`
- 依赖：Python 3.10 或更高版本，仅使用标准库

当前版本按固定 24 字节大端帧头接收 RA8P1 JPEG 图像，并严格按照 `jpeg_length` 累计读取数据。
接收器会检查 JPEG 的 `FF D8` 与 `FF D9` 标志，将图片按 `ra8p1_YYYYMMDD_HHMMSS_mmm.jpg` 保存到 `ERROR` 目录，并在网页中实时显示最新图片、置信度和本次运行的异常历史。

帧头字段依次为 `magic/version/header_size/flags/frame_id/width/height/jpeg_length/confidence_milli/reserved`。

如果提示端口被占用，请先关闭此前运行的 PowerShell TCP 服务端，再重新启动本程序。
