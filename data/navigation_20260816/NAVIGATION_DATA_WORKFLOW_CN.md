# 车辆自动防跌落数据全量重标与训练操作手册

## 1. 本手册解决什么问题

本手册用于管理车辆自动防跌落的黑白二值数据，并完成“保留原始证据、重新标注全部有效样本、训练浅层决策树、检查漏判样本”的完整闭环喵。

当前目标只覆盖普通太阳能板表面、太阳能板外边缘，以及明确允许直向通过的两板接缝喵。

“斜向跨越两块太阳能板”已经被定义为目标范围之外的数据，必须继续保留在原始数据中用于追溯，但不得进入人工标注、训练和验证喵。

本流程不会过滤原始采集行，也不会因为某个画面看起来无用就从 CSV 中删除它喵。

## 2. 当前数据和图像含义

摄像头原始灰度图为 `200 x 112 Gray8`，当前防跌落算法使用底部 `200 x 24` ROI 喵。

二值阈值为 `128`，CSV 中 `bits` 的 `1` 表示暗像素，标注窗口中会显示为黑色喵。

ROI 横向分为三段喵。

| 区域 | 像素列数 | 当前范围 |
|---|---:|---|
| Left | 67 | 1～67 |
| Center | 67 | 68～134 |
| Right | 66 | 135～200 |

各区域暗像素百分比的基本计算为喵。

```text
dark_percent = 100 * dark_pixel_count / region_pixel_count
```

标注时不得使用旧固件的 `65%`、`80%`、连续帧数或 `intent` 作为真值依据，因为这些正是本轮需要重新评估的旧算法参数喵。

车辆俯视几何采用车体左下角为原点、`+x` 向右、`+y` 向前的坐标系喵。

车体尺寸为 `21.5 cm x 21.5 cm`，摄像头坐标为 `(10.75 cm, 26.5 cm)`，即横向居中且位于车体前缘前方 `5.0 cm` 喵。

摄像头高度、俯仰角和像素到地面的单应关系当前尚未确定，因此不能仅凭像素位置计算边缘到车轮的精确物理距离喵。

## 3. 工程目录和启动位置

所有 PowerShell 和 MATLAB 命令默认从下面的工程根目录运行喵。

```text
D:\Lab\Lab_MCU\Renesas_RA\RenesasCupFinalProjectVehicle_20260815\RA8P1_DualCoreProject
```

在 MATLAB 中先切换到这个目录，然后添加工具目录喵。

```matlab
cd('D:\Lab\Lab_MCU\Renesas_RA\RenesasCupFinalProjectVehicle_20260815\RA8P1_DualCoreProject');
addpath('tools');
```

若后续命令报告找不到 `label_navigation_samples`，先运行下面的检查喵。

```matlab
which label_navigation_samples -all
```

正确结果应指向本工程 `tools/label_navigation_samples.m`，而不是其他工程中的同名文件喵。

## 4. 文件职责总表

### 4.1 六份原始采集文件

| 文件 | 用途 | 处理原则 |
|---|---|---|
| `太阳能板边缘中心旋转扫描.txt` | 太阳能板边缘及中心旋转扫描 | 原始证据，只读保留 |
| `左转极限.txt` | 左转接近极限状态 | 原始证据，只读保留 |
| `safe.txt` | 正常受支撑表面及环境反光 | 原始证据，只读保留 |
| `两太阳能板过度.txt` | 非斜向的两板过渡数据 | 原始证据，只读保留 |
| `两太阳能板带角度过度.txt` | 斜向跨越两板 | 保留原文，但整段排除 |
| `太阳能板直角边源.txt` | 多段直角边缘和过渡数据 | 原始证据，其中第 8 段排除 |

这六份文件当前位于 `C:\Users\lingk\Desktop`，其 SHA-256、行数和来源路径已经记录在 `manifest.csv` 中喵。

不要用记事本、Excel 或脚本覆盖这些 TXT，也不要在原文件中删掉断连日志、残缺快照或看起来无效的行喵。

### 4.2 预处理和算法脚本

| 文件 | 职责 | 是否人工修改 |
|---|---|---|
| `tools/preprocess_navigation_rtt.ps1` | 无过滤地解析 RTT 文本并生成统一 CSV | 正常操作时不要改 |
| `tools/analyze_navigation_data.m` | 从统一 CSV 生成原始波形、直方图和二值示例 | 正常操作时不要改 |
| `tools/build_navigation_features.m` | 生成 78 个空间特征和 8 个时间变化率，并维护标签模板 | 正常操作时不要改 |
| `tools/label_navigation_samples.m` | 交互显示二值快照并保存人工标签 | 正常操作时不要改 |
| `tools/label_navigation_motion.m` | 标注采集画面对应的真实车辆运动上下文 | 正常操作时不要改 |
| `tools/train_navigation_tree.m` | 训练不依赖 Statistics Toolbox 的浅层 CART | 得到基线结果前不要改参数 |

脚本可以由版本控制恢复或由开发人员审查后修改，但在同一轮数据实验中不应边标注边改脚本，否则结果将失去可比性喵。

### 4.3 统一数据文件

| 文件 | 当前规模 | 职责 | 修改规则 |
|---|---:|---|---|
| `manifest.csv` | 6 行 | 原始文件路径、哈希、字节数、行数和解析计数 | 不手工修改 |
| `raw_records.csv` | 14678 行 | 逐行保存全部原始文本和解析类型 | 不手工修改 |
| `nav_metrics.csv` | 743 行 | 解析出的 `[NAV]` 指标、状态和帧号 | 不手工修改 |
| `binary_rows.csv` | 12808 行 | 每个有效二值行及完整 200 bit 载荷 | 不手工修改 |
| `binary_snapshots.csv` | 544 行 | 快照边界、完整性、区域统计和内容哈希 | 不手工修改 |
| `snapshot_duplicates.csv` | 330 行 | 标识内容完全相同的重复快照 | 不手工修改 |
| `navigation_features.csv` | 544 行 | 快照元数据、排除状态及 86 个派生特征 | 不手工修改，可重建 |
| `navigation_labels.csv` | 358 行 | 独立完整图像的安全真值、运动上下文和备注 | 只能按本手册修改 |
| `navigation_exclusions.csv` | 2 条规则 | 定义必须排除的采集段 | 未改变项目边界前禁止修改 |
| `vehicle_camera_geometry.csv` | 5 行 | 车体和摄像头的顶视坐标 | 只有重新实测后才能修改 |

`content_sha256` 是图像内容主键，`snapshot_id` 是采集快照标识，二者都不得在 Excel 中改写喵。

`source_line` 是原始采集顺序的稳定索引，而 `frame` 可能在设备复位后重新从小值开始，因此不得只用 `frame` 合并不同数据段喵。

`is_canonical=True` 表示同一二值内容只选择一份独立样本，防止重复帧在训练中被多次加权喵。

### 4.4 图片和模型输出

| 路径或文件 | 职责 | 是否可删除 |
|---|---|---|
| `plots/raw_d128_by_scenario.png` | 各场景原始 L/C/R 波形 | 可删除，可重建 |
| `plots/center_d128_histograms.png` | 中央暗像素比例直方图 | 可删除，可重建 |
| `plots/safe_binary_examples.png` | `safe.txt` 的二值示例 | 可删除，可重建 |
| `label_exports/*.png` | 标注窗口手工导出的疑难样本 | 分析完成后可删除 |
| `model/navigation_tree_model.mat` | MATLAB 决策树模型结构 | 可删除，可重训 |
| `model/navigation_tree_nodes.csv` | 可审计的树节点、阈值和分支 | 可删除，可重训 |
| `model/navigation_tree_validation.csv` | 每个训练样本的真值和预测 | 可删除，可重训 |
| `model/navigation_tree_metrics.csv` | 训练及分组验证指标 | 可删除，可重训 |
| `model/navigation_tree_misclassified.csv` | 分组验证中的误分类样本 | 可删除，可重训 |

`plots`、`label_exports` 和 `model` 都属于可再生输出，但在完成一次分析并记录结论以前不要急着删除喵。

## 5. 哪些文件一定不能动

以下内容构成数据可追溯性的底线，在没有重新建立完整数据集的情况下不得删除或手工编辑喵。

1. 六份原始 TXT 文件及其内容喵。
2. `manifest.csv` 中记录的原始文件哈希喵。
3. `navigation_exclusions.csv` 中的斜向跨板排除规则喵。
4. CSV 中的 `content_sha256`、`snapshot_id`、`source_file`、`segment` 和 `begin_frame` 等样本身份字段喵。
5. `navigation_labels.csv` 中 `EXCLUDED` 行的标签和排除备注喵。

不要为了重新标注而删除 `navigation_labels.csv`，因为删除后重新生成标签模板时，`safe.txt` 会按初始化规则自动被标成 `SAFE`，这不符合“所有有效样本都重新人工确认”的目标喵。

不要直接使用 Excel 对 CSV 排序后只保存部分列，因为这可能改变字符串、哈希、长数字和行间对应关系喵。

## 6. 当前应该执行的全量重标流程

### 第 1 步：确认工作目录

在 MATLAB 中执行喵。

```matlab
cd('D:\Lab\Lab_MCU\Renesas_RA\RenesasCupFinalProjectVehicle_20260815\RA8P1_DualCoreProject');
addpath('tools');
```

成功标准是 `which label_navigation_samples` 指向当前 Vehicle 工程喵。

### 第 2 步：备份并重置全部有效标签

下面的代码会先创建带时间戳的备份，然后把所有非 `EXCLUDED` 样本重置为 `UNLABELED`，并清空这些样本的旧备注喵。

```matlab
dataDir = 'data/navigation_20260816';
labelsPath = fullfile(dataDir, 'navigation_labels.csv');

T = readtable(labelsPath, ...
    TextType='string', ...
    VariableNamingRule='preserve');

allowedLabels = ["SAFE", "DANGER", "SEAM", "UNCERTAIN", ...
                 "UNLABELED", "EXCLUDED"];
assert(all(ismember(T.ground_truth, allowedLabels)), ...
    '发现未知标签，请停止并检查 navigation_labels.csv');

backupPath = fullfile(dataDir, ...
    ['navigation_labels_backup_' datestr(now, 'yyyymmdd_HHMMSS') '.csv']);
copyfile(labelsPath, backupPath);

resetMask = T.ground_truth ~= "EXCLUDED";
T.ground_truth(resetMask) = "UNLABELED";
T.notes(resetMask) = "";
writetable(T, labelsPath);

fprintf('备份文件：%s\n', backupPath);
fprintf('待重新标注：%d\n', nnz(T.ground_truth == "UNLABELED"));
fprintf('继续排除：%d\n', nnz(T.ground_truth == "EXCLUDED"));
```

当前数据的预期输出是 `待重新标注：315` 和 `继续排除：43` 喵。

如果数量不是 `315/43`，先停止操作并把 MATLAB 输出发给我，不要继续覆盖文件喵。

时间戳备份在本轮重标完成并核对以前不要删除喵。

### 第 3 步：启动从头标注

执行下面的命令喵。

```matlab
label_navigation_samples('data/navigation_20260816', 'unlabeled');
```

标注器只显示非排除且当前为 `UNLABELED` 的独立完整快照喵。

每处理一张都会立即写回 `navigation_labels.csv`，按 `Q` 或直接关闭窗口都会保存后退出喵。

### 第 4 步：理解每个按键

| 按键 | 标签或动作 | 使用条件 |
|---|---|---|
| `S` | `SAFE` | 车辆在该物理状态下具有可靠连续支撑，可以正常继续行驶 |
| `D` | `DANGER` | 已到板外或边缘风险状态，必须触发停车或脱险动作 |
| `C` | `SEAM` | 已确认是允许直向通过、两侧均有可靠支撑的两板接缝 |
| `U` | `UNCERTAIN` | 仅凭记录无法确定当时真实支撑状态 |
| `R` | `UNLABELED` | 撤销当前标签，留待以后重新判断 |
| `B` | 上一张 | 返回前一个样本并允许重新覆盖其标签 |
| `K` | 保持当前标签 | 复核模式下确认原标签正确并前进 |
| `Q` | 保存并退出 | 暂停本轮标注 |

当前全量重标的 `unlabeled` 模式中，通常不需要使用 `K`，因为每张图都应重新作出判断喵。

### 第 5 步：遵守统一标注判据

人工标签表示真实物理安全状态，而不是画面亮暗程度或旧算法是否报警喵。

标注时应遵守以下顺序喵。

1. 先根据采集场景和当时车辆位置判断车辆是否具有可靠支撑喵。
2. 再观察黑白二值空间结构，确认该图是否与物理状态一致喵。
3. 不看旧 `current` 标签作暗示，也不以 `d128` 是否越过旧阈值作决定喵。
4. 环境光、太阳反射或日光灯反射造成的白线仍属于真实输入扰动，不得因为“不干净”而删除样本喵。
5. 无法从采集记录确认真实状态时标 `UNCERTAIN`，不要强行猜成 `SAFE` 或 `DANGER` 喵。
6. 只有明确允许直向跨越的两板接缝才标 `SEAM`，斜向跨板已经由排除规则阻断，不会出现在窗口中喵。

同一物理状态应尽量使用同一判据，否则模型会学到标注者前后不一致，而不是学习防跌落特征喵。

### 第 6 步：中途恢复标注

如果按 `Q` 或关闭窗口，下次仍运行同一条命令即可从剩余 `UNLABELED` 样本继续喵。

```matlab
label_navigation_samples('data/navigation_20260816', 'unlabeled');
```

不要再次执行“备份并重置”代码，否则已经完成的新标签会再次被清空喵。

### 第 7 步：复核已经标好的样本

若发现少数新标签有误，运行下面的复核模式喵。

```matlab
label_navigation_samples('data/navigation_20260816', 'labeled');
```

该模式只显示 `SAFE/DANGER/SEAM/UNCERTAIN`，仍不会显示或改动 `EXCLUDED` 喵。

使用 `K` 保持正确标签，使用 `S/D/C/U` 直接覆盖错误标签，使用 `R` 把无法决定的样本退回待标状态喵。

`all` 模式会同时显示已标注和未标注的全部非排除样本，只有需要顺序检查完整有效集合时才使用喵。

```matlab
label_navigation_samples('data/navigation_20260816', 'all');
```

训练完成后可以只复核分组验证的误分类样本，并优先显示危险漏判喵。

```matlab
label_navigation_samples('data/navigation_20260816', 'misclassified');
```

该模式读取 `model/navigation_tree_misclassified.csv`，先显示 `MISSED_DANGER`，再显示 `FALSE_DANGER`，标题同时给出当前人工标签和分组验证预测喵。

若人工标签正确则按 `K` 保留，若人工标签错误则用 `S/D/C/U` 覆盖，无法确认则按 `R` 退回 `UNLABELED` 喵。

误分类清单是上一次训练的静态结果，完成复核后必须重新运行训练器，旧清单不会自动代表新标签的验证结果喵。

### 第 7.1 步：标注现有样本的运动上下文

运动上下文不是旧视觉状态机输出的 `nav_intent`，而是拍摄该画面时车辆实际正在执行的运动喵。

```matlab
label_navigation_motion('data/navigation_20260816', 'unlabeled');
```

按 `W/S/A/D/X/U` 分别标注 `FORWARD/REVERSE/TURN_LEFT/TURN_RIGHT/STATIONARY/UNKNOWN`，按 `B` 返回上一张重新判断，按 `Q` 或关闭窗口会保存退出喵。

如果无法根据原采集过程确认实际运动，必须标 `UNKNOWN`，不能根据旧算法输出猜测喵。

参与训练的 `SAFE/DANGER` 样本中，已知运动上下文覆盖率达到 `90%` 且至少包含两种已知运动时，训练器会加入五个运动 one-hot 特征喵。

`UNKNOWN` 样本使用全零运动向量，因此无法确认的动作不需要强行猜测；若覆盖率不足，训练器会回退到纯图像模式并在命令行说明原因喵。

运动上下文完成后再次运行 `train_navigation_tree`，通过同一套采集组留一验证比较是否真正改善喵。

### 第 8 步：检查标注是否完成

运行喵。

```matlab
T = readtable('data/navigation_20260816/navigation_labels.csv', ...
    TextType='string', VariableNamingRule='preserve');

fprintf('SAFE=%d\n',      nnz(T.ground_truth == "SAFE"));
fprintf('DANGER=%d\n',    nnz(T.ground_truth == "DANGER"));
fprintf('SEAM=%d\n',      nnz(T.ground_truth == "SEAM"));
fprintf('UNCERTAIN=%d\n', nnz(T.ground_truth == "UNCERTAIN"));
fprintf('UNLABELED=%d\n', nnz(T.ground_truth == "UNLABELED"));
fprintf('EXCLUDED=%d\n',  nnz(T.ground_truth == "EXCLUDED"));
```

完成标准是 `UNLABELED=0` 且 `EXCLUDED=43` 喵。

`UNCERTAIN` 可以存在，但它不会进入当前静态 `SAFE/DANGER` 决策树训练喵。

如果 `DANGER<8` 或 `SAFE<8`，训练器会主动拒绝训练，因为样本不足以满足当前最小叶节点约束喵。

## 7. 生成特征和图表

原始 TXT 没有改变时，不需要为了重新标注而重新运行预处理喵。

如需确认特征文件与当前脚本一致，可以执行喵。

```matlab
build_navigation_features('data/navigation_20260816');
```

该脚本会重建 `navigation_features.csv`，并按 `content_sha256` 保留已有人工标签，同时重新强制应用 `navigation_exclusions.csv` 喵。

该脚本生成 `86` 个特征，包括全局和 L/C/R 暗像素比例、行列统计、边界白区、内部白区、黑白跳变、最长白段、暗区包围盒、`4 x 5` 网格、白区质心与协方差、三方向边缘拟合，以及按帧号归一化的前后变化率喵。

不要在 Excel 中直接改 `navigation_features.csv` 的数值，因为该文件必须始终能够由 `binary_rows.csv` 确定性重建喵。

如需重建无过滤分析图片，执行喵。

```matlab
analyze_navigation_data('data/navigation_20260816', ...
    'data/navigation_20260816/plots');
```

成功标准是 `plots` 目录中生成三张 PNG，MATLAB 命令行打印 `Plots written to ...` 喵。

## 8. 训练浅层决策树

完成全部标注并检查类别数量后执行喵。

```matlab
train_navigation_tree('data/navigation_20260816', ...
    'data/navigation_20260816/model');
```

当前训练器只使用 `SAFE` 和 `DANGER`，自动排除 `SEAM`、`UNCERTAIN`、`UNLABELED`、`EXCLUDED`、残缺快照和重复图像喵。

当前基线配置是最大深度 `4`、最小叶节点 `8`、危险类别权重 `4.0` 和危险概率阈值 `0.35` 喵。

第一次得到真实标签结果前不要同时修改这些参数，否则无法判断改进来自数据还是参数喵。

训练器按 `source_file + segment` 做留一采集组验证，目的是避免同一段连续采集中的相似画面同时出现在训练侧和验证侧喵。

## 9. 如何判断训练结果是否可用

首先打开 `model/navigation_tree_metrics.csv` 喵。

重点查看 `Leave-one-capture-group-out` 行，而不是只看容易偏高的 `Training` 行喵。

| 指标 | 含义 | 当前最低要求 |
|---|---|---|
| `missed_danger` | 危险被预测为安全 | 必须为 0 |
| `danger_recall` | 危险样本召回率 | 必须为 1.0 |
| `false_danger` | 安全被预测为危险 | 越少越好，但可在安全约束下权衡 |
| `safe_recall` | 安全样本正确通过率 | 在零漏危险前提下尽量提高 |
| `balanced_accuracy` | 两类召回率平均值 | 用于综合比较，不可代替零漏判要求 |

`missed_danger=0` 只是考虑部署的必要条件，不是充分条件喵。

还必须检查验证覆盖数量、采集场景覆盖、误报分布、未参与训练的 `SEAM/UNCERTAIN` 数量，以及新采集的完全独立测试数据喵。

然后打开 `model/navigation_tree_misclassified.csv`，逐条检查误分类样本是否属于标注错误、环境反光、边缘过渡、数据范围缺失或模型能力不足喵。

如果发现标签错误，应先修正 `navigation_labels.csv` 并重新训练，不要先通过改树参数掩盖错误真值喵。

## 10. 本轮完成后需要提供给我的内容

标注完成后，请告诉我已经完成，并提供或保留以下结果供下一步分析喵。

1. MATLAB 标签统计输出，即 `SAFE/DANGER/SEAM/UNCERTAIN/UNLABELED/EXCLUDED` 六个数量喵。
2. 运行 `train_navigation_tree` 后 MATLAB 命令行打印的 Training 和 Leave-one-capture-group-out 指标喵。
3. `data/navigation_20260816/model/navigation_tree_metrics.csv` 喵。
4. `data/navigation_20260816/model/navigation_tree_misclassified.csv` 喵。
5. 对所有 `UNCERTAIN` 样本的物理原因说明，必要时在标注器中使用 `Export PNG` 导出图片喵。
6. 如果某张图无法判断，说明它来自哪个场景、当时车辆是否仍有轮子受支撑、运动方向以及你希望车辆采取的动作喵。

这些信息齐全后，才能决定下一步是修正标注、增加特征、调整决策树代价和阈值，还是补采特定边界数据喵。

当前阶段不要修改 Vehicle 固件中的防跌落阈值或电机门控代码，以免采集定义、离线真值和在线行为同时变化喵。

## 11. 将来加入新采集数据时需要什么

每次新增采集请保留未经编辑的 RTT 原始 TXT，并同时记录以下元数据喵。

| 必需信息 | 示例或说明 |
|---|---|
| 场景名称 | 正常板面、直边接近、直向两板接缝 |
| 真实物理类别 | SAFE、DANGER、SEAM 或待确认 |
| 车辆运动 | 前进、后退、左转、右转、原地旋转 |
| 速度或 PWM | 至少记录控制档位，最好记录实际速度 |
| 板边相对方向 | 与车头夹角及边缘位于左、中、右哪个区域 |
| 光照条件 | 室内灯、太阳直射、太阳反射、阴影切换 |
| 支撑状态 | 哪些车轮仍在板上，是否允许继续运动 |
| 采集边界 | 开始和结束动作，是否发生复位或 RTT 断连 |
| 排除原因 | 若属于斜向跨板等范围外动作，必须明确记录 |

如果能够同步保存原始 Gray8 帧、现场照片或俯视视频，它们会显著提高人工真值可信度，因为二值图本身可能无法区分物理上不同但像素相同的状态喵。

新数据不要求人为清理反光、噪声、断连行或不完整帧，统一预处理负责保留并标记这些情况喵。

### 新数据预处理命令

只有加入新 TXT 或需要从原始证据完全重建统一 CSV 时，才运行 PowerShell 预处理喵。

```powershell
& .\tools\preprocess_navigation_rtt.ps1 `
  -InputFiles `
    'C:\Users\lingk\Desktop\太阳能板边缘中心旋转扫描.txt', `
    'C:\Users\lingk\Desktop\左转极限.txt', `
    'C:\Users\lingk\Desktop\safe.txt', `
    'C:\Users\lingk\Desktop\两太阳能板过度.txt', `
    'C:\Users\lingk\Desktop\两太阳能板带角度过度.txt', `
    'C:\Users\lingk\Desktop\太阳能板直角边源.txt' `
  -OutputDirectory '.\data\navigation_20260816'
```

加入第七份或更多 TXT 时，把新路径追加到 `-InputFiles` 列表，不要替换或覆盖旧 TXT 喵。

预处理完成后应重新运行 `build_navigation_features`，检查新增场景是否需要新的排除规则，然后只标注新增的 `UNLABELED` 独立快照喵。

## 12. 可删除、可修改和禁止操作的最终清单

### 可以直接删除并重新生成

- `data/navigation_20260816/plots/` 喵。
- `data/navigation_20260816/model/` 喵。
- `data/navigation_20260816/label_exports/`，前提是疑难样本分析已经完成喵。

### 可以按规定修改

- `navigation_labels.csv` 的 `ground_truth` 和 `notes`，应优先通过标注器修改喵。
- `vehicle_camera_geometry.csv`，仅在重新测量车体或摄像头位置后修改喵。
- `navigation_exclusions.csv`，仅在明确改变目标运行范围并经过确认后修改喵。
- 工具脚本，仅在保存基线结果、记录修改原因并重新验证后修改喵。

### 不应手工修改，但可由脚本重建

- `manifest.csv` 喵。
- `raw_records.csv` 喵。
- `nav_metrics.csv` 喵。
- `binary_rows.csv` 喵。
- `binary_snapshots.csv` 喵。
- `snapshot_duplicates.csv` 喵。
- `navigation_features.csv` 喵。

### 一定不能随意删除或覆盖

- 六份原始 TXT 喵。
- 当前仍有效的 `navigation_labels.csv` 及本轮重标前的时间戳备份喵。
- `navigation_exclusions.csv` 中的斜向跨板排除规则喵。
- 所有用于关联数据的哈希、快照 ID、源文件名、数据段和帧身份字段喵。

## 13. 出错时的恢复方法

如果误标了少数样本，使用 `labeled` 模式覆盖，不要整表重置喵。

如果误执行了全量重置但还没有开始新标注，可以用时间戳备份覆盖恢复 `navigation_labels.csv` 喵。

如果已经把新旧标签混在一起，先同时保留当前文件和备份文件，不要继续标注，再根据 `content_sha256` 比较差异喵。

如果标注窗口被关闭，当前标签已经保存，下次运行 `unlabeled` 模式即可继续喵。

如果 MATLAB 报某快照不是 `24 x 200`，不要手工补齐二值数据，应检查该快照是否被错误标记为完整，并把错误日志发给我喵。

如果 `EXCLUDED` 数量不再是 `43`，立即停止训练并检查 `navigation_exclusions.csv`、`navigation_features.csv` 和 `navigation_labels.csv` 的一致性喵。

如果分组验证出现任何 `missed_danger`，当前模型不得写入 Vehicle 固件，应先检查误分类样本和数据覆盖再决定下一步喵。

本手册的核心原则是原始证据不改、排除范围不漂移、人工真值可追溯、每次只改变一个变量，并用独立分组验证结果决定是否继续喵。
