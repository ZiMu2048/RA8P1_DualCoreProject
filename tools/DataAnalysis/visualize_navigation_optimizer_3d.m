function result = visualize_navigation_optimizer_3d(dataDirectory, outputDirectory, showFigure)
%VISUALIZE_NAVIGATION_OPTIMIZER_3D Plot a 2-D slice of the real 25-D loss.
% The red curve is the Nesterov trajectory projected onto its first two
% principal directions. The second panel keeps the exact full-dimensional loss.

if nargin < 1 || strlength(string(dataDirectory)) == 0
    scriptDirectory = fileparts(mfilename('fullpath'));
    dataDirectory = fullfile(scriptDirectory, '..', 'data', 'navigation_20260816');
end
if nargin < 2 || strlength(string(outputDirectory)) == 0
    outputDirectory = fullfile(dataDirectory, 'model_report');
end
if nargin < 3
    showFigure = true;
end
if ~isfolder(outputDirectory)
    mkdir(outputDirectory);
end

fontName = "Microsoft YaHei";
[design, y, sampleWeight, model] = local_load_problem(dataDirectory);
[history, objectiveHistory, lambda, step] = local_run_nesterov( ...
    design, y, sampleWeight, model.lambda_ratio, model.config);
modelBeta = [model.standardized_intercept; model.standardized_coefficients(:)];
reproductionError = norm(history(:, end) - modelBeta, inf);
if reproductionError > 1.0e-6
    error('Optimizer reproduction error %.3g exceeds tolerance.', ...
        reproductionError);
end

center = modelBeta;
centeredHistory = history - center;
[directions, singularValues, ~] = svd(centeredHistory, 'econ');
direction1 = directions(:, 1);
direction2 = directions(:, 2);
coordinates = [direction1.' * centeredHistory; ...
    direction2.' * centeredHistory].';
energy = diag(singularValues).^2;
varianceCaptured = sum(energy(1:min(2, numel(energy)))) / sum(energy);

[axis1, axis2] = local_surface_axes(coordinates);
[grid1, grid2] = meshgrid(axis1, axis2);
gridBeta = center + direction1 * grid1(:).' + direction2 * grid2(:).';
surfaceObjective = reshape(local_objective_many(design, y, sampleWeight, ...
    gridBeta, lambda), size(grid1));

projectedBeta = center + direction1 * coordinates(:, 1).' + ...
    direction2 * coordinates(:, 2).';
projectedObjective = local_objective_many(design, y, sampleWeight, ...
    projectedBeta, lambda).';
plotIndices = local_plot_indices(size(history, 2), 55);

visibility = 'off';
if showFigure
    visibility = 'on';
end
figureHandle = figure(Visible=visibility, Color='white', ...
    Position=[60 60 1700 850]);
layout = tiledlayout(figureHandle, 1, 2, Padding='compact', ...
    TileSpacing='compact');
superTitle = title(layout, '当前24特征逻辑回归：Nesterov梯度下降轨迹', ...
    FontName=fontName, FontSize=18, FontWeight='bold');
superTitle.Color = [0.1 0.1 0.1];

axisHandle = nexttile(layout);
surfaceHandle = surf(axisHandle, grid1, grid2, surfaceObjective, ...
    EdgeColor='none', FaceAlpha=0.86);
surfaceHandle.DiffuseStrength = 0.75;
hold(axisHandle, 'on');
plot3(axisHandle, coordinates(plotIndices, 1), ...
    coordinates(plotIndices, 2), projectedObjective(plotIndices), ...
    '-o', Color=[0.80 0.16 0.12], MarkerFaceColor=[0.80 0.16 0.12], ...
    MarkerEdgeColor='white', LineWidth=2.5, MarkerSize=5);
scatter3(axisHandle, coordinates(1, 1), coordinates(1, 2), ...
    projectedObjective(1), 100, [0.15 0.35 0.75], 'filled', ...
    MarkerEdgeColor='white', LineWidth=1.2);
minimumObjective = local_objective_many(design, y, sampleWeight, ...
    center, lambda);
scatter3(axisHandle, 0, 0, minimumObjective, 120, [0.10 0.58 0.32], ...
    'filled', MarkerEdgeColor='white', LineWidth=1.2);
text(axisHandle, coordinates(1, 1), coordinates(1, 2), ...
    projectedObjective(1), '  初始点', FontName=fontName, ...
    FontSize=11, Color=[0.1 0.1 0.1]);
text(axisHandle, 0, 0, minimumObjective, '  全局最优点', ...
    FontName=fontName, FontSize=11, Color=[0.1 0.1 0.1]);
xlabel(axisHandle, '轨迹主方向1中的参数位移');
ylabel(axisHandle, '轨迹主方向2中的参数位移');
zlabel(axisHandle, '加权交叉熵 + L2正则');
title(axisHandle, sprintf('25维目标函数的二维切片（轨迹信息保留 %.2f%%）', ...
    100 * varianceCaptured));
legend(axisHandle, {'损失曲面', 'Nesterov轨迹投影', '初始点', ...
    '全局最优点'}, Location='northeast');
grid(axisHandle, 'on');
view(axisHandle, -48, 31);
colormap(axisHandle, parula(256));
local_style_axis(axisHandle, fontName);

axisHandle = nexttile(layout);
objectiveGap = abs(objectiveHistory - objectiveHistory(end)) + 1.0e-14;
semilogy(axisHandle, 0:(numel(objectiveHistory) - 1), objectiveGap, ...
    Color=[0.20 0.48 0.72], LineWidth=2.2);
hold(axisHandle, 'on');
scatter(axisHandle, 0, objectiveGap(1), 70, [0.15 0.35 0.75], 'filled');
scatter(axisHandle, numel(objectiveHistory) - 1, objectiveGap(end), ...
    80, [0.10 0.58 0.32], 'filled');
xlabel(axisHandle, '迭代次数');
ylabel(axisHandle, '与最终目标函数的绝对差值 |J(\beta_k)-J_{final}|');
title(axisHandle, sprintf('完整25维空间中的真实收敛过程（%d次迭代）', ...
    numel(objectiveHistory) - 1));
grid(axisHandle, 'on');
local_style_axis(axisHandle, fontName);

pngPath = fullfile(outputDirectory, '05_梯度下降三维轨迹.png');
figPath = fullfile(outputDirectory, '05_梯度下降三维轨迹.fig');
exportgraphics(figureHandle, pngPath, Resolution=180);
savefig(figureHandle, figPath);
if ~showFigure
    close(figureHandle);
end

result.iterations = size(history, 2) - 1;
result.lambda = lambda;
result.step = step;
result.minimum_objective = minimumObjective;
result.trajectory_variance_captured = varianceCaptured;
result.reproduction_error = reproductionError;
result.png_path = string(pngPath);
result.fig_path = string(figPath);
fprintf(['三维优化图已生成：iterations=%d, lambda=%.6g, step=%.6g, ' ...
    'trajectory_capture=%.4f, reproduction_error=%.3g\n'], ...
    result.iterations, lambda, step, varianceCaptured, reproductionError);
end

function [design, y, sampleWeight, model] = local_load_problem(dataDirectory)
loaded = load(fullfile(dataDirectory, 'model_pruned_mcu_simple', ...
    'navigation_pruning_result.mat'));
model = loaded.result.strict_best_model;
if model.feature_count ~= 24
    error('Expected the selected 24-feature model.');
end
features = readtable(fullfile(dataDirectory, 'navigation_features.csv'), ...
    TextType='string', VariableNamingRule='preserve');
labels = readtable(fullfile(dataDirectory, 'navigation_labels.csv'), ...
    TextType='string', VariableNamingRule='preserve');
exclusions = readtable(fullfile(dataDirectory, 'navigation_exclusions.csv'), ...
    TextType='string', VariableNamingRule='preserve');
if height(exclusions) ~= 2 || ...
        ~any(exclusions.source_file == "两太阳能板带角度过度.txt" & ...
        exclusions.segment == 1) || ...
        ~any(exclusions.source_file == "太阳能板直角边源.txt" & ...
        exclusions.segment == 8)
    error('Protected navigation exclusions changed.');
end
validLabel = labels.ground_truth == "SAFE" | ...
    labels.ground_truth == "DANGER";
validLabels = labels(validLabel, :);
[found, labelLocation] = ismember(features.content_sha256, ...
    validLabels.content_sha256);
trainingMask = features.complete & features.is_canonical & ...
    ~features.is_excluded & found;
training = features(trainingMask, :);
trainingLabels = validLabels.ground_truth(labelLocation(trainingMask));
raw = table2array(training(:, cellstr(model.feature_names)));
standardized = (raw - model.standardization_mean) ./ ...
    model.standardization_scale;
design = [ones(size(standardized, 1), 1), standardized];
y = double(trainingLabels == "DANGER");
sampleWeight = ones(size(y));
sampleWeight(y > 0) = model.config.danger_weight;
end

function [history, objectiveHistory, lambda, step] = local_run_nesterov( ...
    design, y, sampleWeight, lambdaRatio, config)
weightSum = sum(sampleWeight);
weightedPrevalence = min(max(sum(sampleWeight .* y) / weightSum, ...
    1.0e-6), 1 - 1.0e-6);
intercept = log(weightedPrevalence / (1 - weightedPrevalence));
X = design(:, 2:end);
residual = sampleWeight .* (y - weightedPrevalence);
lambdaMax = max(abs(X.' * residual)) / weightSum;
lambda = lambdaRatio * max(lambdaMax, 1.0e-8);
weightedDesign = design .* sqrt(sampleWeight);
spectralSquared = local_spectral_norm_squared(weightedDesign);
step = 1 / (0.25 * spectralSquared / weightSum + lambda + 1.0e-12);

beta = [intercept; zeros(size(X, 2), 1)];
momentumPoint = beta;
momentum = 1;
history = zeros(numel(beta), config.solver_max_iterations + 1);
objectiveHistory = zeros(config.solver_max_iterations + 1, 1);
history(:, 1) = beta;
objectiveHistory(1) = local_objective_many(design, y, sampleWeight, ...
    beta, lambda);
for iteration = 1:config.solver_max_iterations
    probability = local_sigmoid(design * momentumPoint);
    gradient = design.' * (sampleWeight .* (probability - y)) / weightSum;
    gradient(2:end) = gradient(2:end) + lambda * momentumPoint(2:end);
    next = momentumPoint - step * gradient;
    nextMomentum = (1 + sqrt(1 + 4 * momentum^2)) / 2;
    accelerated = next + ((momentum - 1) / nextMomentum) * (next - beta);
    history(:, iteration + 1) = next;
    objectiveHistory(iteration + 1) = local_objective_many( ...
        design, y, sampleWeight, next, lambda);
    if norm(next - beta, inf) <= config.solver_tolerance * ...
            (1 + norm(beta, inf))
        break;
    end
    beta = next;
    momentumPoint = accelerated;
    momentum = nextMomentum;
end
history = history(:, 1:(iteration + 1));
objectiveHistory = objectiveHistory(1:(iteration + 1));
end

function objective = local_objective_many(design, y, sampleWeight, beta, lambda)
scores = design * beta;
logisticLoss = max(scores, 0) - y .* scores + log1p(exp(-abs(scores)));
objective = sum(sampleWeight .* logisticLoss, 1) / sum(sampleWeight) + ...
    0.5 * lambda * sum(beta(2:end, :).^2, 1);
end

function [axis1, axis2] = local_surface_axes(coordinates)
minimum = min(coordinates, [], 1);
maximum = max(coordinates, [], 1);
span = maximum - minimum;
span(span < 1.0e-3) = 1;
minimum = min(minimum - 0.20 * span, -0.25 * span);
maximum = max(maximum + 0.20 * span, 0.25 * span);
axis1 = linspace(minimum(1), maximum(1), 75);
axis2 = linspace(minimum(2), maximum(2), 75);
end

function indices = local_plot_indices(count, maximumPoints)
if count <= maximumPoints
    indices = 1:count;
else
    indices = unique(round(linspace(1, count, maximumPoints)));
end
end

function value = local_spectral_norm_squared(X)
vector = ones(size(X, 2), 1);
vector = vector / norm(vector);
for iteration = 1:30
    next = X.' * (X * vector);
    nextNorm = norm(next);
    if nextNorm == 0
        value = 0;
        return;
    end
    vector = next / nextNorm;
end
value = norm(X * vector)^2;
end

function probability = local_sigmoid(score)
score = min(max(score, -40), 40);
probability = 1 ./ (1 + exp(-score));
end

function local_style_axis(axisHandle, fontName)
axisHandle.FontName = fontName;
axisHandle.FontSize = 10;
axisHandle.LineWidth = 0.8;
axisHandle.Box = 'on';
axisHandle.Color = 'white';
axisHandle.XColor = [0.15 0.15 0.15];
axisHandle.YColor = [0.15 0.15 0.15];
axisHandle.ZColor = [0.15 0.15 0.15];
axisHandle.GridColor = [0.72 0.72 0.72];
axisHandle.Title.Color = [0.1 0.1 0.1];
axisHandle.XLabel.Color = [0.15 0.15 0.15];
axisHandle.YLabel.Color = [0.15 0.15 0.15];
axisHandle.ZLabel.Color = [0.15 0.15 0.15];
end
