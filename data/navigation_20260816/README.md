# 车辆防跌落数据集说明与操作入口

本目录保存六份 RTT 原始采集的无过滤统一数据、人工标签、派生特征、分析图表和模型验证结果喵。

预处理不会删除原始行、反光、断连记录、残缺快照或重复画面，只会解析、标记和建立可追溯关系喵。

更完整的备份、恢复、可修改范围和新数据接入说明见 [NAVIGATION_DATA_WORKFLOW_CN.md](NAVIGATION_DATA_WORKFLOW_CN.md) 喵。

## 1. 当前数据规模

| 内容 | 数量 |
|---|---:|
| 原始文本行 | 14678 |
| `[NAV]` 指标记录 | 743 |
| 有效二值行 | 12808 |
| 全部快照 | 544 |
| 完整快照 | 523 |
| 独立完整图像 | 358 |
| 斜向跨板排除图像 | 43 |
| 可标注有效图像 | 315 |

## 2. 文件用途

| 文件 | 用途 | 是否手工修改 |
|---|---|---|
| `manifest.csv` | 原始文件路径、SHA-256、行数和解析统计 | 禁止 |
| `raw_records.csv` | 按原顺序保留每一行采集文本 | 禁止 |
| `nav_metrics.csv` | 解析出的导航状态和 L/C/R 指标 | 禁止 |
| `binary_rows.csv` | 每行原始 `200 bit` 二值载荷 | 禁止 |
| `binary_snapshots.csv` | 快照边界、完整性和区域统计 | 禁止 |
| `snapshot_duplicates.csv` | 完全重复图像的哈希关系 | 禁止 |
| `navigation_features.csv` | `78` 个空间特征和 `8` 个时间变化率 | 禁止，可由脚本重建 |
| `navigation_labels.csv` | 安全真值、运动上下文和人工备注 | 只能通过标注器或受控恢复修改 |
| `navigation_exclusions.csv` | 斜向跨板硬排除规则 | 未改变项目范围前禁止修改 |
| `vehicle_camera_geometry.csv` | 车体与摄像头顶视坐标 | 仅重新实测后修改 |

`content_sha256`、`snapshot_id`、`source_file`、`segment` 和 `begin_frame` 都是样本身份信息，禁止在 Excel 中改写喵。

## 3. 安全标签含义

| 标签 | 含义 |
|---|---|
| `SAFE` | 当前待审核动作可以继续，车辆具有可靠支撑 |
| `DANGER` | 当前待审核动作必须被阻止，否则存在跌落风险 |
| `SEAM` | 已确认可通过的直向两板接缝，当前静态二分类树不使用 |
| `UNCERTAIN` | 无法根据记录确认物理安全状态 |
| `EXCLUDED` | 斜向跨板等目标范围外数据，禁止进入训练和验证 |

安全标签表示“继续当前动作是否安全”，不是旧固件阈值的输出喵。

## 4. 运动上下文含义

`motion_context` 表示防跌落模块正在审核的当前动作，而不是看到危险后应该采取的脱险动作喵。

| 标签 | 判断方法 |
|---|---|
| `FORWARD` | 小车正在前进，或静止摆拍但测试目标是继续前进 |
| `REVERSE` | 小车正在后退，或测试目标是继续后退 |
| `TURN_LEFT` | 小车正在左转，或测试目标是继续左转 |
| `TURN_RIGHT` | 小车正在右转，或测试目标是继续右转 |
| `STATIONARY` | 确实没有待审核的运动动作 |
| `UNKNOWN` | 无法根据采集过程确认，禁止猜测 |

例如小车正在前进但应该停车时，运动上下文仍标 `FORWARD`，安全标签标 `DANGER` 喵。

小车正在右转但应该向左脱险时，运动上下文仍标 `TURN_RIGHT`，不能标成 `TURN_LEFT` 喵。

## 5. MATLAB 启动

```matlab
cd('D:\Lab\Lab_MCU\Renesas_RA\RenesasCupFinalProjectVehicle_20260815\RA8P1_DualCoreProject');
addpath('tools');
```

## 6. 安全标签操作

继续标注未完成安全标签喵。

```matlab
label_navigation_samples('data/navigation_20260816', 'unlabeled');
```

复核全部已标安全标签喵。

```matlab
label_navigation_samples('data/navigation_20260816', 'labeled');
```

只复核上一次分组验证的误分类，危险漏判排在前面喵。

```matlab
label_navigation_samples('data/navigation_20260816', 'misclassified');
```

安全标注按键为 `S=安全、D=危险、C=接缝、U=不确定、R=撤销、B=上一张、K=保持、Q=保存退出` 喵。

## 7. 运动上下文操作

继续标注尚未完成的运动上下文喵。

```matlab
label_navigation_motion('data/navigation_20260816', 'unlabeled');
```

复核已有运动上下文喵。

```matlab
label_navigation_motion('data/navigation_20260816', 'labeled');
```

运动标注按键为 `W=前进、S=后退、A=左转、D=右转、X=静止、U=不确定、R=撤销、B=上一张、K=保持、Q=保存退出` 喵。

两个标注器都会每张立即保存，直接关闭窗口也会保存退出喵。

## 8. 特征重建

只有修改特征脚本、加入新原始数据或需要验证派生文件一致性时，才运行喵。

```matlab
build_navigation_features('data/navigation_20260816');
```

该操作会按 `content_sha256` 保留已有安全标签和运动上下文，并重新强制应用排除规则喵。

## 9. 训练模型

完成标签后运行喵。

```matlab
train_navigation_tree( ...
    'data/navigation_20260816', ...
    'data/navigation_20260816/model');
```

当前模型是传统监督式机器学习中的浅层 CART 决策树，不是神经网络、YOLO、在线学习或训练后量化喵。

模型在 MATLAB 中离线训练，部署时只需要执行固定特征计算和少量阈值比较，不需要第二套 AI 推理运行时喵。

运动标签覆盖率达到 `90%` 且至少包含两种已知运动时，训练器会启用五个 one-hot 运动特征喵。

`UNKNOWN` 样本使用全零运动向量，不要求为了启用模型而伪造运动标签喵。

### 9.1 正则化线性危险评分器

浅层树不能满足分组验证的零危险漏判要求时，运行下面的离线比较喵。

```matlab
train_navigation_linear( ...
    'data/navigation_20260816', ...
    'data/navigation_20260816/model_linear');
```

该脚本不依赖 Statistics and Machine Learning Toolbox，也不会修改 Vehicle 固件喵。

它比较纯图像岭模型、带动作主效应的岭模型、稀疏弹性网络模型，以及“动作 one-hot × 图像特征”的弹性网络交互模型喵。

所有候选都使用 `source_file + segment` 分组，标准化均值和标准差只从当前训练折计算，验证组不参与标准化喵。

候选和门限严格按 `missed_danger` 最少、`false_danger` 最少、`balanced_accuracy` 最高的顺序选择喵。

如果多个门限的混淆矩阵完全相同，脚本选择离最近样本分数最远的门限，降低后续定点量化时贴边翻转的风险喵。

`model_linear/navigation_linear_candidates.csv` 是所有候选在汇总留组分数上的消融比较，并包含有效系数数量喵。

`model_linear/navigation_linear_nested_metrics.csv` 是更严格的嵌套分组验证结果，应作为当前离线模型选择流程的正式指标喵。

嵌套验证的外层采集组完全不参与候选和门限选择，内层只使用其余采集组重新完成模型与门限选择喵。

`model_linear/navigation_linear_nested_folds.csv` 记录每个外层组选择的模型、门限和混淆计数喵。

`model_linear/navigation_linear_validation.csv` 保存汇总留组分数、嵌套分数、每折门限、动作上下文和最终 STOP 判定，可用于定位跨组误差喵。

`model_linear/navigation_linear_coefficients.csv` 和 `navigation_linear_model.mat` 保存完整数据拟合后的浮点模型，后续定点转换必须逐样本对照这些参考结果喵。

危险评分小于 `safe_permission_threshold` 时才允许当前动作，达到该门限就必须 STOP 喵。

`danger_threshold=0.5` 只把已经 STOP 的样本进一步显示为 `UNKNOWN` 或 `DANGER`，不改变安全仲裁结果，也没有独立的安全保证喵。

当前阶段即使嵌套验证达到零漏判，也只表示可以继续做特征裁剪、定点量化和逐样本等价验证，不能直接写入 Vehicle 固件喵。

### 9.2 面向 MCU 的特征裁剪

运行仅使用简单空间特征的裁剪验证喵。

```matlab
analyze_navigation_pruning( ...
    'data/navigation_20260816', ...
    'data/navigation_20260816/model_pruned_mcu_simple', ...
    'mcu_simple');
```

`mcu_simple` 会排除时间变化率、标准差、质心、协方差、拟合斜率和拟合残差，只保留容易由整数扫描得到的特征喵。

每个外层采集组的特征排名、正则强度和安全门限都只使用其余采集组决定喵。

当前严格嵌套结果中，24 特征模型为 `TD=114、MD=0、FD=19、TS=178`，继续增加到 32、48 或 62 个简单特征都会产生 1 个危险漏判喵。

因此当前 MCU 主候选固定为 24 个简单空间特征，不能根据训练集表现改用更大的模型喵。

### 9.3 定点评分验证

运行定点格式扫描喵。

```matlab
quantize_navigation_pruned_model( ...
    'data/navigation_20260816', ...
    'data/navigation_20260816/model_pruned_mcu_simple/navigation_pruning_result.mat', ...
    'data/navigation_20260816/model_fixed_mcu_simple');
```

当前主模型使用特征 F4、系数 F14 和 `int64` F18 累加器喵。

311 张参考样本上没有浮点到定点判定翻转，最大 logit 误差为 `0.00744`，最小浮点判定裕量为 `0.03045` 喵。

MCU 端不需要计算 sigmoid，只需比较整数累加器与整数门限喵。

主机 C 参考实现位于 `tools/navigation_mcu_reference`，其 311 张图验证结果要求特征、累加器和判定全部逐位一致喵。

该验证仍未覆盖在线图像缓存、ROI 行列方向、二值极性、实时执行时间和 M33 动作仲裁，因此尚不能修改现有电机控制与 IPC 行为喵。

## 10. 查看训练结果

训练后首先打开 `model/navigation_tree_metrics.csv`，重点查看 `Leave-one-capture-group-out` 行喵。

最低安全要求是 `missed_danger=0` 和 `danger_recall=1.0`，但这只是考虑部署的必要条件喵。

然后打开 `model/navigation_tree_misclassified.csv`，逐张检查危险漏判和安全误报喵。

`model/navigation_tree_threshold_sweep.csv` 给出分组验证在 `0～1` 各危险概率阈值下的漏判和误报，步长为 `0.001`，用于判断零漏判需要付出多少停车代价喵。

阈值扫描使用现有验证集做诊断，不能单独证明新阈值对未来未知场景同样有效喵。

`model` 目录中的文件均可通过重新训练生成，但在记录本轮结论以前不要删除喵。

线性评分器必须优先查看 `model_linear/navigation_linear_nested_metrics.csv`，不能只引用候选表中使用同一批汇总留组预测选择出的较乐观指标喵。

## 11. 重新生成无过滤图表

```matlab
analyze_navigation_data( ...
    'data/navigation_20260816', ...
    'data/navigation_20260816/plots');
```

`plots`、`label_exports`、`model` 和 `model_linear` 属于可再生输出，原始 TXT、标签备份、排除规则和样本主键不能随意删除喵。
