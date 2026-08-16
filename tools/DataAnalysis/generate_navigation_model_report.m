function generate_navigation_model_report(dataDirectory, outputDirectory)
%GENERATE_NAVIGATION_MODEL_REPORT Generate Chinese model-analysis figures.

if nargin < 1 || strlength(string(dataDirectory)) == 0
    scriptDirectory = fileparts(mfilename('fullpath'));
    dataDirectory = fullfile(scriptDirectory, '..', 'data', 'navigation_20260816');
end
if nargin < 2 || strlength(string(outputDirectory)) == 0
    outputDirectory = fullfile(dataDirectory, 'model_report');
end
if ~isfolder(outputDirectory)
    mkdir(outputDirectory);
end

fontName = "Microsoft YaHei";
treeMetrics = readtable(fullfile(dataDirectory, 'model', ...
    'navigation_tree_metrics.csv'), TextType='string', ...
    VariableNamingRule='preserve');
linearNested = readtable(fullfile(dataDirectory, 'model_linear', ...
    'navigation_linear_nested_metrics.csv'), TextType='string', ...
    VariableNamingRule='preserve');
pruning = readtable(fullfile(dataDirectory, 'model_pruned_mcu_simple', ...
    'navigation_pruning_curve.csv'), TextType='string', ...
    VariableNamingRule='preserve');
folds = readtable(fullfile(dataDirectory, 'model_pruned_mcu_simple', ...
    'navigation_pruning_nested_folds.csv'), TextType='string', ...
    VariableNamingRule='preserve');
coefficients = readtable(fullfile(dataDirectory, ...
    'model_pruned_mcu_simple', 'navigation_pruning_full_coefficients.csv'), ...
    TextType='string', VariableNamingRule='preserve');
stability = readtable(fullfile(dataDirectory, 'model_pruned_mcu_simple', ...
    'navigation_pruning_feature_stability.csv'), TextType='string', ...
    VariableNamingRule='preserve');
fixedValidation = readtable(fullfile(dataDirectory, ...
    'model_fixed_mcu_simple', 'navigation_fixed_point_validation.csv'), ...
    TextType='string', VariableNamingRule='preserve');
fixedSweep = readtable(fullfile(dataDirectory, 'model_fixed_mcu_simple', ...
    'navigation_fixed_point_sweep.csv'), TextType='string', ...
    VariableNamingRule='preserve');

local_performance_figure(treeMetrics, linearNested, pruning, folds, ...
    fontName, fullfile(outputDirectory, '01_模型性能总览.png'));
local_feature_figure(coefficients, stability, fontName, ...
    fullfile(outputDirectory, '02_特征权重与稳定性.png'));
local_fixed_point_figure(fixedValidation, fixedSweep, fontName, ...
    fullfile(outputDirectory, '03_定点等价性.png'));
local_pipeline_figure(fontName, ...
    fullfile(outputDirectory, '04_MCU推理流程.png'));
fprintf('中文模型报告图已生成到：%s\n', outputDirectory);
end

function local_performance_figure(treeMetrics, linearNested, pruning, folds, ...
    fontName, outputPath)
finalRow = pruning(pruning.feature_count == 24, :);
methodNames = ["浅层树-训练集"; "浅层树-分组验证"; ...
    "86特征线性-嵌套验证"; "24特征简化模型-分组外层预测"];
missed = [treeMetrics.missed_danger(1); treeMetrics.missed_danger(2); ...
    linearNested.missed_danger(1); finalRow.nested_missed_danger];
falseDanger = [treeMetrics.false_danger(1); treeMetrics.false_danger(2); ...
    linearNested.false_danger(1); finalRow.nested_false_danger];
dangerRecall = 100 * [treeMetrics.danger_recall(1); ...
    treeMetrics.danger_recall(2); linearNested.danger_recall(1); ...
    finalRow.nested_danger_recall];
safeRecall = 100 * [treeMetrics.safe_recall(1); ...
    treeMetrics.safe_recall(2); linearNested.safe_recall(1); ...
    finalRow.nested_safe_recall];

figureHandle = figure(Visible='off', Color='white', Position=[50 50 1680 1120]);
layout = tiledlayout(figureHandle, 2, 2, Padding='compact', ...
    TileSpacing='compact');
superTitle = title(layout, '防跌落模型性能与复杂度对比', FontName=fontName, ...
    FontSize=18, FontWeight='bold');
superTitle.Color = [0.1 0.1 0.1];

axisHandle = nexttile(layout);
bars = bar(axisHandle, [missed, falseDanger], 'grouped');
bars(1).FaceColor = [0.80 0.23 0.19];
bars(2).FaceColor = [0.20 0.48 0.72];
grid(axisHandle, 'on');
axisHandle.XTickLabel = methodNames;
axisHandle.XTickLabelRotation = 18;
ylabel(axisHandle, '样本数（张）');
title(axisHandle, '危险漏判与安全误停（越低越好）');
legend(axisHandle, {'危险漏判', '安全误停'}, Location='northwest');
local_style_axis(axisHandle, fontName);
local_label_bars(axisHandle, bars);

axisHandle = nexttile(layout);
bars = bar(axisHandle, [dangerRecall, safeRecall], 'grouped');
bars(1).FaceColor = [0.16 0.60 0.38];
bars(2).FaceColor = [0.89 0.57 0.12];
grid(axisHandle, 'on');
axisHandle.XTickLabel = methodNames;
axisHandle.XTickLabelRotation = 18;
ylim(axisHandle, [0 105]);
ylabel(axisHandle, '召回率（%）');
title(axisHandle, '危险召回率与安全召回率');
legend(axisHandle, {'危险召回率', '安全召回率'}, Location='southwest');
local_style_axis(axisHandle, fontName);
local_label_bars(axisHandle, bars, '%.1f');

axisHandle = nexttile(layout);
yyaxis(axisHandle, 'left');
plot(axisHandle, pruning.feature_count, pruning.nested_missed_danger, ...
    '-o', LineWidth=2, MarkerSize=6, Color=[0.80 0.23 0.19]);
ylabel(axisHandle, '危险漏判（张）');
ylim(axisHandle, [0 max(9, max(pruning.nested_missed_danger) + 1)]);
yyaxis(axisHandle, 'right');
plot(axisHandle, pruning.feature_count, pruning.nested_false_danger, ...
    '-s', LineWidth=2, MarkerSize=6, Color=[0.20 0.48 0.72]);
ylabel(axisHandle, '安全误停（张）');
xlabel(axisHandle, '保留的简单空间特征数');
title(axisHandle, '特征裁剪曲线');
grid(axisHandle, 'on');
xline(axisHandle, 24, '--', '选定24特征', LabelOrientation='horizontal', ...
    FontName=fontName, Color=[0.15 0.15 0.15]);
local_style_axis(axisHandle, fontName);

axisHandle = nexttile(layout);
selectedFolds = folds(folds.feature_count == 24, :);
groupLabels = local_group_labels(selectedFolds.held_out_group);
bars = bar(axisHandle, [selectedFolds.missed_danger, ...
    selectedFolds.false_danger], 'grouped');
bars(1).FaceColor = [0.80 0.23 0.19];
bars(2).FaceColor = [0.20 0.48 0.72];
axisHandle.XTickLabel = groupLabels;
axisHandle.XTickLabelRotation = 35;
ylabel(axisHandle, '样本数（张）');
title(axisHandle, '24特征模型：各留出采集组错误分布');
legend(axisHandle, {'危险漏判', '安全误停'}, Location='northwest');
grid(axisHandle, 'on');
local_style_axis(axisHandle, fontName);

exportgraphics(figureHandle, outputPath, Resolution=180);
close(figureHandle);
end

function local_feature_figure(coefficients, stability, fontName, outputPath)
selected = coefficients(coefficients.feature_count == 24, :);
stable = stability(stability.feature_count == 24, :);
[found, location] = ismember(selected.feature_name, stable.feature_name);
if ~all(found)
    error('Feature stability table is missing selected features.');
end
frequency = stable.selection_frequency(location) * 100;
labels = local_feature_labels(selected.feature_name);

figureHandle = figure(Visible='off', Color='white', Position=[50 50 1650 1250]);
layout = tiledlayout(figureHandle, 1, 2, Padding='compact', ...
    TileSpacing='compact');
superTitle = title(layout, '最终24个简单空间特征', FontName=fontName, ...
    FontSize=18, FontWeight='bold');
superTitle.Color = [0.1 0.1 0.1];

axisHandle = nexttile(layout);
values = selected.standardized_coefficient;
bars = barh(axisHandle, values, FaceColor='flat');
bars.CData = repmat([0.20 0.48 0.72], numel(values), 1);
bars.CData(values > 0, :) = repmat([0.89 0.57 0.12], nnz(values > 0), 1);
axisHandle.YTick = 1:height(selected);
axisHandle.YTickLabel = labels;
axisHandle.YDir = 'reverse';
xline(axisHandle, 0, '-', Color=[0.2 0.2 0.2]);
xlabel(axisHandle, '标准化后的线性权重');
title(axisHandle, '权重方向与大小');
grid(axisHandle, 'on');
local_style_axis(axisHandle, fontName);

axisHandle = nexttile(layout);
barh(axisHandle, frequency, FaceColor=[0.16 0.60 0.38]);
axisHandle.YTick = 1:height(selected);
axisHandle.YTickLabel = labels;
axisHandle.YDir = 'reverse';
xlim(axisHandle, [0 105]);
xlabel(axisHandle, '13个外层折中的入选比例（%）');
title(axisHandle, '跨采集组特征稳定性');
grid(axisHandle, 'on');
local_style_axis(axisHandle, fontName);

exportgraphics(figureHandle, outputPath, Resolution=180);
close(figureHandle);
end

function local_fixed_point_figure(validation, sweep, fontName, outputPath)
floatLogit = validation.float_logit_strict;
fixedLogit = validation.fixed_logit_strict;
errorValue = validation.fixed_error_strict;
isDanger = validation.ground_truth == "DANGER";
selected = sweep(sweep.model_name == "STRICT" & ...
    sweep.input_fraction_bits == 4 & ...
    sweep.coefficient_fraction_bits == 14, :);
loaded = load(fullfile(fileparts(fileparts(outputPath)), ...
    'model_fixed_mcu_simple', 'navigation_fixed_point_models.mat'));
threshold = loaded.fixedPoint.strict_model.safe_permission_logit_threshold;

figureHandle = figure(Visible='off', Color='white', Position=[50 50 1600 1100]);
layout = tiledlayout(figureHandle, 2, 2, Padding='compact', ...
    TileSpacing='compact');
superTitle = title(layout, '24特征模型的定点等价性验证', FontName=fontName, ...
    FontSize=18, FontWeight='bold');
superTitle.Color = [0.1 0.1 0.1];

axisHandle = nexttile(layout);
histogram(axisHandle, floatLogit(~isDanger), 24, FaceColor=[0.20 0.48 0.72], ...
    FaceAlpha=0.65, EdgeColor='none');
hold(axisHandle, 'on');
histogram(axisHandle, floatLogit(isDanger), 24, FaceColor=[0.80 0.23 0.19], ...
    FaceAlpha=0.65, EdgeColor='none');
xline(axisHandle, threshold, '--k', sprintf('STOP门限 %.3f', threshold), ...
    FontName=fontName, LabelOrientation='horizontal');
xlabel(axisHandle, '完整数据拟合模型的 logit 分数');
ylabel(axisHandle, '样本数（张）');
title(axisHandle, 'SAFE 与 DANGER 分数分布');
legend(axisHandle, {'SAFE', 'DANGER'}, Location='northwest');
grid(axisHandle, 'on');
local_style_axis(axisHandle, fontName);

axisHandle = nexttile(layout);
scatter(axisHandle, floatLogit(~isDanger), fixedLogit(~isDanger), 24, ...
    [0.20 0.48 0.72], 'filled');
hold(axisHandle, 'on');
scatter(axisHandle, floatLogit(isDanger), fixedLogit(isDanger), 24, ...
    [0.80 0.23 0.19], 'filled');
limits = [min([floatLogit; fixedLogit]), max([floatLogit; fixedLogit])];
plot(axisHandle, limits, limits, '--', Color=[0.15 0.15 0.15]);
xlabel(axisHandle, '浮点 logit');
ylabel(axisHandle, '定点还原 logit');
title(axisHandle, '浮点与定点逐样本对比');
axis(axisHandle, 'equal');
xlim(axisHandle, limits);
ylim(axisHandle, limits);
grid(axisHandle, 'on');
local_style_axis(axisHandle, fontName);

axisHandle = nexttile(layout);
histogram(axisHandle, errorValue, 28, FaceColor=[0.16 0.60 0.38], ...
    EdgeColor='none');
xline(axisHandle, 0, '-', Color=[0.15 0.15 0.15]);
xlabel(axisHandle, '定点 logit - 浮点 logit');
ylabel(axisHandle, '样本数（张）');
title(axisHandle, sprintf('量化误差：最大值 %.5f', selected.max_logit_error));
grid(axisHandle, 'on');
local_style_axis(axisHandle, fontName);

axisHandle = nexttile(layout);
margin = abs(floatLogit - threshold);
[sortedMargin, order] = sort(margin);
pointColors = repmat([0.20 0.48 0.72], numel(margin), 1);
pointColors(isDanger(order), :) = repmat([0.80 0.23 0.19], ...
    nnz(isDanger), 1);
scatter(axisHandle, 1:numel(sortedMargin), sortedMargin, 18, ...
    pointColors, 'filled');
yline(axisHandle, selected.max_logit_error, '--', ...
    '本格式最大定点误差', FontName=fontName);
xlabel(axisHandle, '按判定裕量排序的样本');
ylabel(axisHandle, '|logit - STOP门限|');
title(axisHandle, sprintf('最小浮点裕量 %.5f，判定翻转 %d 张', ...
    selected.minimum_float_margin, selected.decision_flips));
grid(axisHandle, 'on');
local_style_axis(axisHandle, fontName);

exportgraphics(figureHandle, outputPath, Resolution=180);
close(figureHandle);
end

function local_pipeline_figure(fontName, outputPath)
figureHandle = figure(Visible='off', Color='white', Position=[50 50 1800 650]);
annotation(figureHandle, 'textbox', [0.18 0.91 0.64 0.07], ...
    String='固件中的最终推理数据流：一个分数、一个门限、两个分支', ...
    EdgeColor='none', HorizontalAlignment='center', FontName=fontName, ...
    FontSize=18, FontWeight='bold', Color=[0.1 0.1 0.1]);
boxX = [0.03 0.17 0.31 0.45 0.59 0.73];
labels = {sprintf('Gray8图像\n200×112'), sprintf('底部ROI\n200×24'), ...
    sprintf('阈值128二值化\n1 = 暗像素'), ...
    sprintf('单次扫描\n计数 / 极值 / 白段'), sprintf('24个F4特征\n无历史帧'), ...
    sprintf('24次整数乘加\nint64 F18')};
for index = 1:numel(boxX)
    annotation(figureHandle, 'textbox', [boxX(index) 0.43 0.105 0.19], ...
        String=labels{index}, HorizontalAlignment='center', ...
        VerticalAlignment='middle', FontName=fontName, FontSize=11, ...
        Color=[0.1 0.1 0.1], EdgeColor=[0.20 0.48 0.72], ...
        LineWidth=1.8, BackgroundColor=[0.92 0.96 0.99]);
    if index < numel(boxX)
        annotation(figureHandle, 'arrow', ...
            [boxX(index) + 0.105, boxX(index + 1)], [0.525 0.525], ...
            Color=[0.20 0.48 0.72], LineWidth=1.8);
    end
end
annotation(figureHandle, 'textbox', [0.855 0.43 0.11 0.19], ...
    String=sprintf('score_q ≥ -237271 ?'), HorizontalAlignment='center', ...
    VerticalAlignment='middle', FontName=fontName, FontSize=11, ...
    Color=[0.1 0.1 0.1], EdgeColor=[0.80 0.23 0.19], ...
    LineWidth=1.8, BackgroundColor=[0.99 0.93 0.92]);
annotation(figureHandle, 'arrow', [0.835 0.855], [0.525 0.525], ...
    Color=[0.20 0.48 0.72], LineWidth=1.8);
annotation(figureHandle, 'textbox', [0.84 0.70 0.14 0.13], ...
    String=sprintf('否：ALLOW\n允许当前动作'), HorizontalAlignment='center', ...
    VerticalAlignment='middle', FontName=fontName, FontSize=11, ...
    Color=[0.1 0.1 0.1], EdgeColor=[0.16 0.60 0.38], ...
    LineWidth=1.8, BackgroundColor=[0.92 0.98 0.94]);
annotation(figureHandle, 'textbox', [0.84 0.16 0.14 0.13], ...
    String=sprintf('是：STOP\n拒绝当前动作'), HorizontalAlignment='center', ...
    VerticalAlignment='middle', FontName=fontName, FontSize=11, ...
    Color=[0.1 0.1 0.1], EdgeColor=[0.80 0.23 0.19], ...
    LineWidth=1.8, BackgroundColor=[0.99 0.93 0.92]);
annotation(figureHandle, 'arrow', [0.91 0.91], [0.62 0.70], ...
    Color=[0.16 0.60 0.38], LineWidth=1.8);
annotation(figureHandle, 'arrow', [0.91 0.91], [0.43 0.29], ...
    Color=[0.80 0.23 0.19], LineWidth=1.8);
exportgraphics(figureHandle, outputPath, Resolution=180);
close(figureHandle);
end

function local_style_axis(axisHandle, fontName)
axisHandle.FontName = fontName;
axisHandle.FontSize = 10;
axisHandle.LineWidth = 0.8;
axisHandle.Box = 'on';
axisHandle.Color = 'white';
axisHandle.XColor = [0.15 0.15 0.15];
axisHandle.YColor = [0.15 0.15 0.15];
axisHandle.GridColor = [0.70 0.70 0.70];
axisHandle.Title.Color = [0.1 0.1 0.1];
axisHandle.XLabel.Color = [0.15 0.15 0.15];
axisHandle.YLabel.Color = [0.15 0.15 0.15];
end

function local_label_bars(axisHandle, bars, formatText)
if nargin < 3
    formatText = '%.0f';
end
for series = 1:numel(bars)
    x = bars(series).XEndPoints;
    y = bars(series).YEndPoints;
    labels = compose(formatText, bars(series).YData);
    text(axisHandle, x, y, labels, HorizontalAlignment='center', ...
        VerticalAlignment='bottom', FontSize=9, Color=[0.1 0.1 0.1]);
end
end

function labels = local_group_labels(groupNames)
labels = strings(size(groupNames));
for index = 1:numel(groupNames)
    value = groupNames(index);
    value = replace(value, "太阳能板边缘中心旋转扫描.txt|", "旋转扫描-");
    value = replace(value, "左转极限.txt|", "左转极限-");
    value = replace(value, "safe.txt|", "安全板面-");
    value = replace(value, "两太阳能板过度.txt|", "直向接缝-");
    value = replace(value, "太阳能板直角边源.txt|", "直角边-");
    labels(index) = value;
end
end

function labels = local_feature_labels(names)
labels = strings(size(names));
for index = 1:numel(names)
    switch names(index)
        case "center_column_dark_min", labels(index) = "中心列最小暗比例";
        case "grid_dark_r3_c1", labels(index) = "网格R3C1暗比例";
        case "left_edge_valid_percent", labels(index) = "左边界有效行比例";
        case "grid_dark_r1_c5", labels(index) = "网格R1C5暗比例";
        case "dark_left_edge", labels(index) = "最左20列暗比例";
        case "grid_dark_r1_c1", labels(index) = "网格R1C1暗比例";
        case "right_edge_valid_percent", labels(index) = "右边界有效行比例";
        case "row_dark_max", labels(index) = "单行最大暗比例";
        case "dark_right_edge", labels(index) = "最右20列暗比例";
        case "grid_dark_r3_c2", labels(index) = "网格R3C2暗比例";
        case "dark_left", labels(index) = "左区域暗比例";
        case "column_dark_min", labels(index) = "单列最小暗比例";
        case "white_right_border_max", labels(index) = "右侧最大连续白边";
        case "dark_bottom_half", labels(index) = "下半区暗比例";
        case "longest_white_run_mean", labels(index) = "各行最长白段均值";
        case "grid_dark_r4_c1", labels(index) = "网格R4C1暗比例";
        case "grid_dark_r2_c3", labels(index) = "网格R2C3暗比例";
        case "grid_dark_r1_c4", labels(index) = "网格R1C4暗比例";
        case "dark_all", labels(index) = "ROI总体暗比例";
        case "dark_right", labels(index) = "右区域暗比例";
        case "center_row_dark_max", labels(index) = "中心区单行最大暗比例";
        case "white_top_border_mean", labels(index) = "顶部连续白边均值";
        case "white_right_border_mean", labels(index) = "右侧连续白边均值";
        case "row_transition_max", labels(index) = "单行最大黑白跳变数";
        otherwise, labels(index) = names(index);
    end
end
end
