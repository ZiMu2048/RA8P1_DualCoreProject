function model = train_navigation_tree(dataDirectory, outputDirectory)
%TRAIN_NAVIGATION_TREE Train an auditable shallow CART classifier.
% This implementation does not require Statistics and Machine Learning Toolbox.

if nargin < 1 || strlength(string(dataDirectory)) == 0
    scriptDirectory = fileparts(mfilename('fullpath'));
    dataDirectory = fullfile(scriptDirectory, '..', 'data', 'navigation_20260816');
end
if nargin < 2 || strlength(string(outputDirectory)) == 0
    outputDirectory = fullfile(dataDirectory, 'model');
end
if ~isfolder(outputDirectory)
    mkdir(outputDirectory);
end

config.max_depth = 4;
config.min_leaf_samples = 8;
config.min_gain = 1.0e-4;
config.danger_weight = 4.0;
config.danger_probability_threshold = 0.35;
config.min_motion_coverage = 0.90;

features = readtable(fullfile(dataDirectory, 'navigation_features.csv'), ...
    TextType='string', VariableNamingRule='preserve');
labels = readtable(fullfile(dataDirectory, 'navigation_labels.csv'), ...
    TextType='string', VariableNamingRule='preserve');

validLabel = labels.ground_truth == "SAFE" | labels.ground_truth == "DANGER";
labels = labels(validLabel, :);
if nnz(labels.ground_truth == "SAFE") < config.min_leaf_samples || ...
        nnz(labels.ground_truth == "DANGER") < config.min_leaf_samples
    error(['At least %d independently labeled SAFE and DANGER samples are ' ...
        'required. Current labels: SAFE=%d DANGER=%d.'], ...
        config.min_leaf_samples, nnz(labels.ground_truth == "SAFE"), ...
        nnz(labels.ground_truth == "DANGER"));
end

[found, labelLocation] = ismember(features.content_sha256, labels.content_sha256);
trainingMask = features.complete & features.is_canonical & ...
    ~features.is_excluded & found;
training = features(trainingMask, :);
trainingLabels = labels.ground_truth(labelLocation(trainingMask));
y = trainingLabels == "DANGER";

firstFeature = find(strcmp(features.Properties.VariableNames, 'dark_all'), 1);
featureNames = string(features.Properties.VariableNames(firstFeature:end));
X = table2array(training(:, cellstr(featureNames)));
if any(~isfinite(X), 'all')
    error('Training features contain NaN or Inf values.');
end

motionContext = repmat("UNAVAILABLE", height(training), 1);
motionEnabled = false;
motionCoverage = 0;
motionClasses = ["FORWARD", "REVERSE", "TURN_LEFT", "TURN_RIGHT", ...
    "STATIONARY"];
if ismember('motion_context', labels.Properties.VariableNames)
    motionContext = labels.motion_context(labelLocation(trainingMask));
    knownMotion = ismember(motionContext, motionClasses);
    motionCoverage = nnz(knownMotion) / height(training);
    knownClassCount = numel(unique(motionContext(knownMotion)));
    if motionCoverage >= config.min_motion_coverage && knownClassCount >= 2
        motionX = zeros(height(training), numel(motionClasses));
        for motionIndex = 1:numel(motionClasses)
            motionX(:, motionIndex) = ...
                double(motionContext == motionClasses(motionIndex));
        end
        X = [X, motionX];
        featureNames = [featureNames, "motion_" + motionClasses];
        motionEnabled = true;
        fprintf(['Motion context enabled with %d one-hot features: ' ...
            '%d/%d known (%.1f%%), UNKNOWN uses all zeros.\n'], ...
            numel(motionClasses), nnz(knownMotion), height(training), ...
            100 * motionCoverage);
    else
        fprintf(['Motion context disabled: %d/%d known (%.1f%%), need >=%.1f%% ' ...
            'and at least two known motion classes.\n'], ...
            nnz(knownMotion), height(training), 100 * motionCoverage, ...
            100 * config.min_motion_coverage);
    end
end

nodes = local_grow_tree(X, y, featureNames, config);
[predicted, trainingProbability] = local_predict_tree(nodes, X, ...
    config.danger_probability_threshold);
trainingMetrics = local_calculate_metrics("Training", y, predicted);
local_print_metrics(trainingMetrics);

captureGroup = training.source_file + "|" + string(training.segment);
validationPrediction = repmat("NOT_EVALUATED", height(training), 1);
validationProbability = nan(height(training), 1);
for heldOutGroup = unique(captureGroup).'
    testMask = captureGroup == heldOutGroup;
    trainMask = ~testMask;
    if numel(unique(y(trainMask))) < 2 || ...
            nnz(trainMask) < 2 * config.min_leaf_samples
        continue;
    end
    foldNodes = local_grow_tree(X(trainMask, :), y(trainMask), featureNames, config);
    [foldPrediction, foldProbability] = local_predict_tree( ...
        foldNodes, X(testMask, :), ...
        config.danger_probability_threshold);
    validationPrediction(testMask) = local_prediction_names(foldPrediction);
    validationProbability(testMask) = foldProbability;
end

evaluated = validationPrediction ~= "NOT_EVALUATED";
if any(evaluated)
    validationMetrics = local_calculate_metrics( ...
        "Leave-one-capture-group-out", y(evaluated), ...
        validationPrediction(evaluated) == "DANGER");
    local_print_metrics(validationMetrics);
    metricRows = [trainingMetrics; validationMetrics];
else
    warning('Group validation was not possible with the current label distribution.');
    metricRows = trainingMetrics;
end

model.config = config;
model.feature_names = featureNames;
model.nodes = nodes;
model.motion_context_enabled = motionEnabled;
model.motion_classes = motionClasses;
model.motion_context_coverage = motionCoverage;
model.motion_unknown_encoding = "ALL_ZERO";
save(fullfile(outputDirectory, 'navigation_tree_model.mat'), 'model');

nodeTable = struct2table(nodes);
writetable(nodeTable, fullfile(outputDirectory, 'navigation_tree_nodes.csv'));
validation = training(:, {'content_sha256', 'source_file', 'scenario', ...
    'segment', 'begin_frame'});
validation.ground_truth = trainingLabels;
validation.motion_context = motionContext;
validation.training_danger_probability = trainingProbability;
validation.training_prediction = local_prediction_names(predicted);
validation.group_validation_danger_probability = validationProbability;
validation.group_validation_prediction = validationPrediction;
writetable(validation, fullfile(outputDirectory, ...
    'navigation_tree_validation.csv'));
writetable(struct2table(metricRows), fullfile(outputDirectory, ...
    'navigation_tree_metrics.csv'));
misclassified = validation(evaluated & ...
    validation.group_validation_prediction ~= validation.ground_truth, :);
writetable(misclassified, fullfile(outputDirectory, ...
    'navigation_tree_misclassified.csv'));
if any(evaluated)
    thresholdSweep = local_threshold_sweep(y(evaluated), ...
        validationProbability(evaluated));
    writetable(thresholdSweep, fullfile(outputDirectory, ...
        'navigation_tree_threshold_sweep.csv'));
    bestIndex = local_best_threshold_index(thresholdSweep);
    fprintf(['Conservative validation threshold=%.2f: MD=%d FD=%d ' ...
        'danger_recall=%.3f safe_recall=%.3f.\n'], ...
        thresholdSweep.threshold(bestIndex), ...
        thresholdSweep.missed_danger(bestIndex), ...
        thresholdSweep.false_danger(bestIndex), ...
        thresholdSweep.danger_recall(bestIndex), ...
        thresholdSweep.safe_recall(bestIndex));
end

fprintf('Tree nodes=%d, depth<=%d. Model written to %s\n', ...
    numel(nodes), config.max_depth, outputDirectory);
end

function nodes = local_grow_tree(X, y, featureNames, config)
emptyNode = struct('id', 0, 'depth', 0, 'is_leaf', true, ...
    'prediction', "SAFE", 'danger_probability', 0, 'sample_count', 0, ...
    'danger_count', 0, 'feature_index', 0, 'feature_name', "", ...
    'threshold', NaN, 'left_id', 0, 'right_id', 0, 'gain', 0);
nodes = repmat(emptyNode, 0, 1);
[nodes, ~] = local_add_node(nodes, X, y, featureNames, config, 0);
end

function [nodes, nodeId] = local_add_node(nodes, X, y, featureNames, config, depth)
nodeId = numel(nodes) + 1;
dangerProbability = (sum(y) + 1) / (numel(y) + 2);
node = struct('id', nodeId, 'depth', depth, 'is_leaf', true, ...
    'prediction', local_class_name(dangerProbability, ...
        config.danger_probability_threshold), ...
    'danger_probability', dangerProbability, 'sample_count', numel(y), ...
    'danger_count', sum(y), 'feature_index', 0, 'feature_name', "", ...
    'threshold', NaN, 'left_id', 0, 'right_id', 0, 'gain', 0);
nodes(nodeId) = node;

stop = depth >= config.max_depth || ...
       numel(y) < 2 * config.min_leaf_samples || ...
       all(y == y(1));
if stop
    return;
end

[featureIndex, threshold, gain, leftMask] = local_best_split(X, y, config);
if featureIndex == 0 || gain < config.min_gain
    return;
end

[nodes, leftId] = local_add_node(nodes, X(leftMask, :), y(leftMask), ...
    featureNames, config, depth + 1);
[nodes, rightId] = local_add_node(nodes, X(~leftMask, :), y(~leftMask), ...
    featureNames, config, depth + 1);
node.is_leaf = false;
node.feature_index = featureIndex;
node.feature_name = featureNames(featureIndex);
node.threshold = threshold;
node.left_id = leftId;
node.right_id = rightId;
node.gain = gain;
nodes(nodeId) = node;
end

function [bestFeature, bestThreshold, bestGain, bestLeft] = ...
    local_best_split(X, y, config)
weights = ones(size(y));
weights(y) = config.danger_weight;
parentImpurity = local_weighted_gini(y, weights);
bestFeature = 0;
bestThreshold = NaN;
bestGain = 0;
bestLeft = false(size(y));

for feature = 1:size(X, 2)
    values = unique(X(:, feature));
    if numel(values) < 2
        continue;
    end
    thresholds = (values(1:end-1) + values(2:end)) / 2;
    for threshold = thresholds.'
        left = X(:, feature) <= threshold;
        leftCount = nnz(left);
        rightCount = numel(y) - leftCount;
        if leftCount < config.min_leaf_samples || ...
                rightCount < config.min_leaf_samples
            continue;
        end
        leftWeight = sum(weights(left));
        rightWeight = sum(weights(~left));
        totalWeight = leftWeight + rightWeight;
        childImpurity = ...
            (leftWeight / totalWeight) * local_weighted_gini(y(left), weights(left)) + ...
            (rightWeight / totalWeight) * local_weighted_gini(y(~left), weights(~left));
        gain = parentImpurity - childImpurity;
        if gain > bestGain
            bestFeature = feature;
            bestThreshold = threshold;
            bestGain = gain;
            bestLeft = left;
        end
    end
end
end

function impurity = local_weighted_gini(y, weights)
totalWeight = sum(weights);
if totalWeight <= 0
    impurity = 0;
    return;
end
dangerFraction = sum(weights(y)) / totalWeight;
impurity = 1 - dangerFraction^2 - (1 - dangerFraction)^2;
end

function [predicted, probability] = local_predict_tree(nodes, X, dangerThreshold)
predicted = false(size(X, 1), 1);
probability = zeros(size(X, 1), 1);
for row = 1:size(X, 1)
    nodeId = 1;
    while ~nodes(nodeId).is_leaf
        if X(row, nodes(nodeId).feature_index) <= nodes(nodeId).threshold
            nodeId = nodes(nodeId).left_id;
        else
            nodeId = nodes(nodeId).right_id;
        end
    end
    probability(row) = nodes(nodeId).danger_probability;
    predicted(row) = probability(row) >= dangerThreshold;
end
end

function sweep = local_threshold_sweep(actualDanger, probability)
thresholds = (0:0.001:1).';
rowCount = numel(thresholds);
sweep = table(thresholds, zeros(rowCount, 1), zeros(rowCount, 1), ...
    zeros(rowCount, 1), zeros(rowCount, 1), zeros(rowCount, 1), ...
    zeros(rowCount, 1), zeros(rowCount, 1), zeros(rowCount, 1), ...
    VariableNames={'threshold', 'evaluated_count', 'true_danger', ...
    'missed_danger', 'false_danger', 'true_safe', 'danger_recall', ...
    'safe_recall', 'balanced_accuracy'});
for index = 1:rowCount
    metrics = local_calculate_metrics("Threshold sweep", actualDanger, ...
        probability >= thresholds(index));
    sweep.evaluated_count(index) = metrics.evaluated_count;
    sweep.true_danger(index) = metrics.true_danger;
    sweep.missed_danger(index) = metrics.missed_danger;
    sweep.false_danger(index) = metrics.false_danger;
    sweep.true_safe(index) = metrics.true_safe;
    sweep.danger_recall(index) = metrics.danger_recall;
    sweep.safe_recall(index) = metrics.safe_recall;
    sweep.balanced_accuracy(index) = metrics.balanced_accuracy;
end
end

function bestIndex = local_best_threshold_index(sweep)
candidates = find(sweep.missed_danger == min(sweep.missed_danger));
minimumFalseDanger = min(sweep.false_danger(candidates));
candidates = candidates(sweep.false_danger(candidates) == minimumFalseDanger);
[~, relativeIndex] = max(sweep.balanced_accuracy(candidates));
bestIndex = candidates(relativeIndex);
end

function name = local_class_name(dangerProbability, threshold)
if dangerProbability >= threshold
    name = "DANGER";
else
    name = "SAFE";
end
end

function metrics = local_calculate_metrics(label, actualDanger, predictedDanger)
trueDanger = nnz(actualDanger & predictedDanger);
missedDanger = nnz(actualDanger & ~predictedDanger);
falseDanger = nnz(~actualDanger & predictedDanger);
trueSafe = nnz(~actualDanger & ~predictedDanger);
dangerRecall = trueDanger / max(1, trueDanger + missedDanger);
safeRecall = trueSafe / max(1, trueSafe + falseDanger);
metrics = struct( ...
    'evaluation', label, ...
    'evaluated_count', numel(actualDanger), ...
    'true_danger', trueDanger, ...
    'missed_danger', missedDanger, ...
    'false_danger', falseDanger, ...
    'true_safe', trueSafe, ...
    'danger_recall', dangerRecall, ...
    'safe_recall', safeRecall, ...
    'balanced_accuracy', (dangerRecall + safeRecall) / 2);
end

function local_print_metrics(metrics)
fprintf(['%s: TD=%d MD=%d FD=%d TS=%d danger_recall=%.3f ' ...
    'safe_recall=%.3f balanced_accuracy=%.3f\n'], ...
    metrics.evaluation, metrics.true_danger, metrics.missed_danger, ...
    metrics.false_danger, metrics.true_safe, metrics.danger_recall, ...
    metrics.safe_recall, metrics.balanced_accuracy);
end

function names = local_prediction_names(predictedDanger)
names = repmat("SAFE", numel(predictedDanger), 1);
names(predictedDanger) = "DANGER";
end
