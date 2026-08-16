# 防跌落 MCU 参考实现

本目录是主机端 C 参考实现，不属于当前 Vehicle 固件喵。

输入是行优先排列的 `24 x 200` 二值 ROI，每个像素使用 `uint8_t`，`1` 表示暗像素喵。

`navigation_extract_features_q` 在一次图像扫描中计算 24 个简单空间特征，输出采用 4 位小数的定点格式，即真实特征值乘以 16 后四舍五入喵。

这些特征只使用计数、比例、极值、连续白段、黑白跳变和固定网格，不依赖浮点、平方根、协方差、直线拟合或历史帧喵。

`navigation_score_q` 执行 24 次 `int32_t x int32_t -> int64_t` 乘加，累加器具有 18 位小数喵。

判定规则是 `score_q >= -237271` 时要求 STOP，否则允许当前动作继续喵。

主机测试命令如下喵。

```powershell
matlab -batch "addpath('tools'); export_navigation_c_test_vectors('data/navigation_20260816','data/navigation_20260816/model_fixed_mcu_simple/navigation_fixed_point_models.mat','data/navigation_20260816/model_fixed_mcu_simple/navigation_c_test_vectors.bin');"

D:\mingw64\bin\gcc.exe -std=c11 -O2 -Wall -Wextra -Werror `
  tools/navigation_mcu_reference/navigation_mcu_reference.c `
  tools/navigation_mcu_reference/navigation_mcu_reference_test.c `
  -o tools/navigation_mcu_reference/navigation_mcu_reference_test.exe

tools/navigation_mcu_reference/navigation_mcu_reference_test.exe `
  data/navigation_20260816/model_fixed_mcu_simple/navigation_c_test_vectors.bin
```

成功标准如下喵。

```text
samples=311 feature_mismatches=0 max_feature_error=0 score_mismatches=0 decision_mismatches=0
```

移入固件前还必须核对在线 ROI 的行列方向、二值极性、阈值 128、缓存维护、调用内核和动作仲裁接口喵。
