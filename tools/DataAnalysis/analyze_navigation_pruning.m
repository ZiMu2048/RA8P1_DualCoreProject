function result = analyze_navigation_pruning(dataDirectory, outputDirectory, featureMode)
%ANALYZE_NAVIGATION_PRUNING Evaluate MCU-oriented feature-count tradeoffs.
% Feature ranking, regularization selection, and thresholds are fold-local.

if nargin < 1 || strlength(string(dataDirectory)) == 0
    scriptDirectory = fileparts(mfilename('fullpath'));
    dataDirectory = fullfile(scriptDirectory, '..', 'data', 'navigation_20260816');
end
if nargin < 2 || strlength(string(outputDirectory)) == 0
    outputDirectory = fullfile(dataDirectory, 'model_pruned');
end
if nargin < 3 || strlength(string(featureMode)) == 0
    featureMode = "all";
end
if ~isfolder(outputDirectory)
    mkdir(outputDirectory);
end

config.feature_counts = [4, 6, 8, 12, 16, 24, 32, 48, 64, 86];
config.lambda_ratios = [0.03, 0.10, 0.30];
config.ranking_lambda_ratio = 0.03;
config.danger_weight = 4.0;
config.threshold_step = 0.001;
config.solver_max_iterations = 2000;
config.solver_tolerance = 1.0e-8;
config.feature_mode = string(featureMode);

[training, trainingLabels, X, featureNames] = ...
    local_load_training_data(dataDirectory);
if config.feature_mode == "spatial"
    spatialMask = ~startsWith(featureNames, "rate_");
    X = X(:, spatialMask);
    featureNames = featureNames(spatialMask);
elseif config.feature_mode == "mcu_simple"
    excludedTokens = ["std", "centroid", "cov_", "slope", "residual"];
    simpleMask = ~startsWith(featureNames, "rate_");
    for token = excludedTokens
        simpleMask = simpleMask & ~contains(featureNames, token);
    end
    X = X(:, simpleMask);
    featureNames = featureNames(simpleMask);
elseif config.feature_mode ~= "all"
    error('featureMode must be "all", "spatial", or "mcu_simple".');
end
y = double(trainingLabels == "DANGER");
captureGroup = training.source_file + "|" + string(training.segment);
groups = unique(captureGroup, 'stable');
sampleCount = height(training);
featureCountTotal = numel(featureNames);

requestedCounts = config.feature_counts(config.feature_counts <= featureCountTotal);
if requestedCounts(end) ~= featureCountTotal
    requestedCounts(end + 1) = featureCountTotal;
end
requestedCounts = unique(requestedCounts, 'stable');
countCandidateCount = numel(requestedCounts);

curveRows = repmat(local_empty_curve_row(), countCandidateCount, 1);
nestedScore = nan(sampleCount, countCandidateCount);
nestedThreshold = nan(sampleCount, countCandidateCount);
nestedStop = false(sampleCount, countCandidateCount);
nestedLambda = nan(sampleCount, countCandidateCount);
foldRows = repmat(local_empty_fold_row(), 0, 1);
stabilityCount = zeros(countCandidateCount, featureCountTotal);
fullModels = cell(countCandidateCount, 1);
fullCoefficientRows = repmat(local_empty_coefficient_row(), 0, 1);

for countIndex = 1:countCandidateCount
    featureCount = requestedCounts(countIndex);
    fprintf('Pruning candidate %d/%d: %d features.\n', ...
        countIndex, countCandidateCount, featureCount);

    pooledScores = local_group_scores(X, y, captureGroup, groups, ...
        featureCount, config);
    [pooledLambdaIndex, pooledThreshold, pooledMetrics] = ...
        local_select_lambda_threshold(y > 0, pooledScores, config);
    pooledLambda = config.lambda_ratios(pooledLambdaIndex);

    for outerIndex = 1:numel(groups)
        heldOutGroup = groups(outerIndex);
        outerTest = captureGroup == heldOutGroup;
        outerTrain = ~outerTest;
        innerGroups = unique(captureGroup(outerTrain), 'stable');
        innerScores = local_group_scores(X(outerTrain, :), y(outerTrain), ...
            captureGroup(outerTrain), innerGroups, featureCount, config);
        [lambdaIndex, threshold, ~] = local_select_lambda_threshold( ...
            y(outerTrain) > 0, innerScores, config);
        lambdaRatio = config.lambda_ratios(lambdaIndex);

        design = local_ranked_design(X(outerTrain, :), X(outerTest, :), ...
            y(outerTrain), featureCount, config);
        fit = local_fit_logistic(design.trainX, y(outerTrain), ...
            lambdaRatio, config);
        score = local_sigmoid(fit.intercept + ...
            design.testX * fit.coefficients);
        stopRequired = score >= threshold;
        nestedScore(outerTest, countIndex) = score;
        nestedThreshold(outerTest, countIndex) = threshold;
        nestedStop(outerTest, countIndex) = stopRequired;
        nestedLambda(outerTest, countIndex) = lambdaRatio;
        stabilityCount(countIndex, design.selectedIndices) = ...
            stabilityCount(countIndex, design.selectedIndices) + 1;

        metrics = local_calculate_metrics(y(outerTest) > 0, stopRequired);
        foldRow = local_empty_fold_row();
        foldRow.feature_count = featureCount;
        foldRow.held_out_group = heldOutGroup;
        foldRow.test_count = nnz(outerTest);
        foldRow.lambda_ratio = lambdaRatio;
        foldRow.safe_permission_threshold = threshold;
        foldRow.true_danger = metrics.true_danger;
        foldRow.missed_danger = metrics.missed_danger;
        foldRow.false_danger = metrics.false_danger;
        foldRow.true_safe = metrics.true_safe;
        foldRows(end + 1) = foldRow; %#ok<AGROW>
    end

    evaluated = isfinite(nestedScore(:, countIndex)) & ...
        isfinite(nestedThreshold(:, countIndex));
    nestedMetrics = local_calculate_metrics(y(evaluated) > 0, ...
        nestedStop(evaluated, countIndex));
    curveRow = local_empty_curve_row();
    curveRow.feature_count = featureCount;
    curveRow.pooled_lambda_ratio = pooledLambda;
    curveRow.pooled_safe_permission_threshold = pooledThreshold;
    curveRow.pooled_missed_danger = pooledMetrics.missed_danger;
    curveRow.pooled_false_danger = pooledMetrics.false_danger;
    curveRow.nested_evaluated_count = nestedMetrics.evaluated_count;
    curveRow.nested_true_danger = nestedMetrics.true_danger;
    curveRow.nested_missed_danger = nestedMetrics.missed_danger;
    curveRow.nested_false_danger = nestedMetrics.false_danger;
    curveRow.nested_true_safe = nestedMetrics.true_safe;
    curveRow.nested_danger_recall = nestedMetrics.danger_recall;
    curveRow.nested_safe_recall = nestedMetrics.safe_recall;
    curveRow.nested_balanced_accuracy = nestedMetrics.balanced_accuracy;
    curveRows(countIndex) = curveRow;

    fullDesign = local_ranked_design(X, X, y, featureCount, config);
    fullFit = local_fit_logistic(fullDesign.trainX, y, pooledLambda, config);
    fullModel = local_export_model(fullDesign, fullFit, featureNames, ...
        pooledThreshold, featureCount, pooledLambda, config);
    fullModels{countIndex} = fullModel;
    for selectedIndex = 1:featureCount
        coefficientRow = local_empty_coefficient_row();
        coefficientRow.feature_count = featureCount;
        coefficientRow.rank = selectedIndex;
        originalIndex = fullDesign.selectedIndices(selectedIndex);
        coefficientRow.feature_name = featureNames(originalIndex);
        coefficientRow.standardization_mean = fullDesign.mean(selectedIndex);
        coefficientRow.standardization_scale = fullDesign.scale(selectedIndex);
        coefficientRow.standardized_coefficient = ...
            fullFit.coefficients(selectedIndex);
        coefficientRow.raw_coefficient = fullModel.raw_coefficients(selectedIndex);
        fullCoefficientRows(end + 1) = coefficientRow; %#ok<AGROW>
    end

    fprintf(['  Nested: MD=%d FD=%d safe_recall=%.3f; ' ...
        'pooled lambda=%.2f threshold=%.3f.\n'], ...
        nestedMetrics.missed_danger, nestedMetrics.false_danger, ...
        nestedMetrics.safe_recall, pooledLambda, pooledThreshold);
end

curve = struct2table(curveRows);
strictIndex = local_best_curve_index(curve);
zeroMiss = find(curve.nested_missed_danger == 0);
if isempty(zeroMiss)
    smallestZeroIndex = 0;
else
    [~, relativeIndex] = min(curve.feature_count(zeroMiss));
    smallestZeroIndex = zeroMiss(relativeIndex);
end

validation = training(:, {'content_sha256', 'source_file', 'scenario', ...
    'segment', 'begin_frame'});
validation.ground_truth = trainingLabels;
for countIndex = 1:countCandidateCount
    suffix = "k" + string(requestedCounts(countIndex));
    validation.("nested_score_" + suffix) = nestedScore(:, countIndex);
    validation.("nested_threshold_" + suffix) = ...
        nestedThreshold(:, countIndex);
    validation.("nested_stop_" + suffix) = nestedStop(:, countIndex);
    validation.("nested_lambda_" + suffix) = nestedLambda(:, countIndex);
end

stabilityRows = repmat(struct('feature_count', 0, 'feature_name', "", ...
    'selected_outer_folds', 0, 'selection_frequency', 0), ...
    countCandidateCount * featureCountTotal, 1);
rowIndex = 0;
for countIndex = 1:countCandidateCount
    for featureIndex = 1:featureCountTotal
        rowIndex = rowIndex + 1;
        stabilityRows(rowIndex).feature_count = requestedCounts(countIndex);
        stabilityRows(rowIndex).feature_name = featureNames(featureIndex);
        stabilityRows(rowIndex).selected_outer_folds = ...
            stabilityCount(countIndex, featureIndex);
        stabilityRows(rowIndex).selection_frequency = ...
            stabilityCount(countIndex, featureIndex) / numel(groups);
    end
end
stability = struct2table(stabilityRows);

result.config = config;
result.curve = curve;
result.strict_best_index = strictIndex;
result.strict_best_model = fullModels{strictIndex};
result.smallest_zero_miss_index = smallestZeroIndex;
if smallestZeroIndex > 0
    result.smallest_zero_miss_model = fullModels{smallestZeroIndex};
else
    result.smallest_zero_miss_model = struct([]);
end
result.deployment_recommendation = ...
    "OFFLINE_ONLY_REVIEW_CURVE_BEFORE_FIXED_POINT";
save(fullfile(outputDirectory, 'navigation_pruning_result.mat'), 'result');
writetable(curve, fullfile(outputDirectory, 'navigation_pruning_curve.csv'));
writetable(struct2table(foldRows), fullfile(outputDirectory, ...
    'navigation_pruning_nested_folds.csv'));
writetable(validation, fullfile(outputDirectory, ...
    'navigation_pruning_validation.csv'));
writetable(stability, fullfile(outputDirectory, ...
    'navigation_pruning_feature_stability.csv'));
writetable(struct2table(fullCoefficientRows), fullfile(outputDirectory, ...
    'navigation_pruning_full_coefficients.csv'));

strict = curve(strictIndex, :);
fprintf('Strict best: K=%d MD=%d FD=%d balanced_accuracy=%.3f.\n', ...
    strict.feature_count, strict.nested_missed_danger, ...
    strict.nested_false_danger, strict.nested_balanced_accuracy);
if smallestZeroIndex > 0
    smallest = curve(smallestZeroIndex, :);
    fprintf(['Smallest zero-miss: K=%d MD=%d FD=%d ' ...
        'balanced_accuracy=%.3f.\n'], smallest.feature_count, ... %#ok<NBRAK2>
        smallest.nested_missed_danger, smallest.nested_false_danger, ...
        smallest.nested_balanced_accuracy);
else
    fprintf('No tested feature count achieved zero nested missed danger.\n');
end
fprintf('No firmware was modified. Output: %s\n', outputDirectory);
end

function scores = local_group_scores(X, y, captureGroup, groups, ...
    featureCount, config)
scores = nan(size(X, 1), numel(config.lambda_ratios));
for heldOutGroup = groups.'
    testMask = captureGroup == heldOutGroup;
    trainMask = ~testMask;
    if numel(unique(y(trainMask))) < 2
        continue;
    end
    design = local_ranked_design(X(trainMask, :), X(testMask, :), ...
        y(trainMask), featureCount, config);
    for lambdaIndex = 1:numel(config.lambda_ratios)
        fit = local_fit_logistic(design.trainX, y(trainMask), ...
            config.lambda_ratios(lambdaIndex), config);
        scores(testMask, lambdaIndex) = local_sigmoid( ...
            fit.intercept + design.testX * fit.coefficients);
    end
end
end

function design = local_ranked_design(trainX, testX, y, featureCount, config)
meanValue = mean(trainX, 1);
scaleValue = std(trainX, 0, 1);
scaleValue(~isfinite(scaleValue) | scaleValue < 1.0e-12) = 1;
trainZ = (trainX - meanValue) ./ scaleValue;
testZ = (testX - meanValue) ./ scaleValue;
rankingFit = local_fit_logistic(trainZ, y, ...
    config.ranking_lambda_ratio, config);
[~, ranking] = sort(abs(rankingFit.coefficients), 'descend');
selected = ranking(1:featureCount);
design.trainX = trainZ(:, selected);
design.testX = testZ(:, selected);
design.selectedIndices = selected;
design.mean = meanValue(selected);
design.scale = scaleValue(selected);
end

function model = local_export_model(design, fit, featureNames, threshold, ...
    featureCount, lambdaRatio, config)
rawCoefficients = fit.coefficients ./ design.scale(:);
rawIntercept = fit.intercept - sum(fit.coefficients .* ...
    (design.mean(:) ./ design.scale(:)));
thresholdClamped = min(max(threshold, 1.0e-9), 1 - 1.0e-9);
logitThreshold = log(thresholdClamped / (1 - thresholdClamped));
standardizedLogit = fit.intercept + design.testX * fit.coefficients;
rawTestX = design.testX .* design.scale + design.mean;
rawLogit = rawIntercept + rawTestX * rawCoefficients;
equivalenceError = max(abs(standardizedLogit - rawLogit));
if equivalenceError > 1.0e-9
    error('Raw-domain model conversion error %.3g exceeds tolerance.', ...
        equivalenceError);
end
model.family = "PRUNED_REGULARIZED_LINEAR_DANGER_SCORER";
model.feature_count = featureCount;
model.feature_names = featureNames(design.selectedIndices);
model.original_feature_indices = design.selectedIndices;
model.standardization_mean = design.mean;
model.standardization_scale = design.scale;
model.standardized_intercept = fit.intercept;
model.standardized_coefficients = fit.coefficients;
model.raw_intercept = rawIntercept;
model.raw_coefficients = rawCoefficients;
model.safe_permission_threshold = threshold;
model.safe_permission_logit_threshold = logitThreshold;
model.lambda_ratio = lambdaRatio;
model.raw_conversion_max_error = equivalenceError;
model.mcu_decision = ...
    "ALLOW iff raw_intercept + sum(raw_coefficient .* feature) < logit_threshold";
model.config = config;
end

function [lambdaIndex, threshold, metrics] = ...
    local_select_lambda_threshold(actualDanger, scores, config)
lambdaRows = repmat(struct('lambda_index', 0, 'missed_danger', 0, ...
    'false_danger', 0, 'balanced_accuracy', 0, 'threshold', 0), ...
    numel(config.lambda_ratios), 1);
for index = 1:numel(config.lambda_ratios)
    evaluated = isfinite(scores(:, index));
    sweep = local_threshold_sweep(actualDanger(evaluated), ...
        scores(evaluated, index), config.threshold_step);
    thresholdIndex = local_best_threshold_index(sweep);
    lambdaRows(index).lambda_index = index;
    lambdaRows(index).missed_danger = sweep.missed_danger(thresholdIndex);
    lambdaRows(index).false_danger = sweep.false_danger(thresholdIndex);
    lambdaRows(index).balanced_accuracy = ...
        sweep.balanced_accuracy(thresholdIndex);
    lambdaRows(index).threshold = sweep.threshold(thresholdIndex);
end
lambdaTable = struct2table(lambdaRows);
candidates = find(lambdaTable.missed_danger == ...
    min(lambdaTable.missed_danger));
candidates = candidates(lambdaTable.false_danger(candidates) == ...
    min(lambdaTable.false_danger(candidates)));
candidates = candidates(lambdaTable.balanced_accuracy(candidates) == ...
    max(lambdaTable.balanced_accuracy(candidates)));
[~, relativeIndex] = max(config.lambda_ratios(candidates));
lambdaIndex = candidates(relativeIndex);
threshold = lambdaTable.threshold(lambdaIndex);
predicted = scores(:, lambdaIndex) >= threshold;
evaluated = isfinite(scores(:, lambdaIndex));
metrics = local_calculate_metrics(actualDanger(evaluated), ...
    predicted(evaluated));
end

function fit = local_fit_logistic(X, y, lambdaRatio, config)
sampleWeight = ones(size(y));
sampleWeight(y > 0) = config.danger_weight;
weightSum = sum(sampleWeight);
weightedPrevalence = min(max(sum(sampleWeight .* y) / weightSum, ...
    1.0e-6), 1 - 1.0e-6);
intercept = log(weightedPrevalence / (1 - weightedPrevalence));
residual = sampleWeight .* (y - weightedPrevalence);
lambdaMax = max(abs(X.' * residual)) / weightSum;
lambda = lambdaRatio * max(lambdaMax, 1.0e-8);
augmented = [ones(size(X, 1), 1), X];
weightedAugmented = augmented .* sqrt(sampleWeight);
spectralSquared = local_spectral_norm_squared(weightedAugmented);
step = 1 / (0.25 * spectralSquared / weightSum + lambda + 1.0e-12);
beta = [intercept; zeros(size(X, 2), 1)];
momentumPoint = beta;
momentum = 1;
for iteration = 1:config.solver_max_iterations
    probability = local_sigmoid(augmented * momentumPoint);
    gradient = augmented.' * (sampleWeight .* (probability - y)) / weightSum;
    gradient(2:end) = gradient(2:end) + lambda * momentumPoint(2:end);
    next = momentumPoint - step * gradient;
    nextMomentum = (1 + sqrt(1 + 4 * momentum^2)) / 2;
    accelerated = next + ((momentum - 1) / nextMomentum) * (next - beta);
    if norm(next - beta, inf) <= config.solver_tolerance * ...
            (1 + norm(beta, inf))
        beta = next;
        break;
    end
    beta = next;
    momentumPoint = accelerated;
    momentum = nextMomentum;
end
fit.intercept = beta(1);
fit.coefficients = beta(2:end);
fit.lambda = lambda;
fit.iterations = iteration;
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

function sweep = local_threshold_sweep(actualDanger, probability, step)
thresholds = (0:step:1).';
rowCount = numel(thresholds);
sweep = table(thresholds, zeros(rowCount, 1), zeros(rowCount, 1), ...
    zeros(rowCount, 1), zeros(rowCount, 1), zeros(rowCount, 1), ...
    zeros(rowCount, 1), zeros(rowCount, 1), zeros(rowCount, 1), ...
    zeros(rowCount, 1), ...
    VariableNames={'threshold', 'evaluated_count', 'true_danger', ...
    'missed_danger', 'false_danger', 'true_safe', 'danger_recall', ...
    'safe_recall', 'balanced_accuracy', 'decision_margin'});
for index = 1:rowCount
    predictedDanger = probability >= thresholds(index);
    metrics = local_calculate_metrics(actualDanger, predictedDanger);
    sweep{index, 2:end} = [metrics.evaluated_count, metrics.true_danger, ...
        metrics.missed_danger, metrics.false_danger, metrics.true_safe, ...
        metrics.danger_recall, metrics.safe_recall, ...
        metrics.balanced_accuracy, min(abs(probability - thresholds(index)))];
end
end

function bestIndex = local_best_threshold_index(sweep)
candidates = find(sweep.missed_danger == min(sweep.missed_danger));
candidates = candidates(sweep.false_danger(candidates) == ...
    min(sweep.false_danger(candidates)));
candidates = candidates(sweep.balanced_accuracy(candidates) == ...
    max(sweep.balanced_accuracy(candidates)));
[~, relativeIndex] = max(sweep.decision_margin(candidates));
bestIndex = candidates(relativeIndex);
end

function metrics = local_calculate_metrics(actualDanger, stopRequired)
trueDanger = nnz(actualDanger & stopRequired);
missedDanger = nnz(actualDanger & ~stopRequired);
falseDanger = nnz(~actualDanger & stopRequired);
trueSafe = nnz(~actualDanger & ~stopRequired);
dangerRecall = trueDanger / max(1, trueDanger + missedDanger);
safeRecall = trueSafe / max(1, trueSafe + falseDanger);
metrics = struct('evaluated_count', numel(actualDanger), ...
    'true_danger', trueDanger, 'missed_danger', missedDanger, ...
    'false_danger', falseDanger, 'true_safe', trueSafe, ...
    'danger_recall', dangerRecall, 'safe_recall', safeRecall, ...
    'balanced_accuracy', (dangerRecall + safeRecall) / 2);
end

function index = local_best_curve_index(curve)
candidates = find(curve.nested_missed_danger == ...
    min(curve.nested_missed_danger));
candidates = candidates(curve.nested_false_danger(candidates) == ...
    min(curve.nested_false_danger(candidates)));
candidates = candidates(curve.nested_balanced_accuracy(candidates) == ...
    max(curve.nested_balanced_accuracy(candidates)));
[~, relativeIndex] = min(curve.feature_count(candidates));
index = candidates(relativeIndex);
end

function [training, trainingLabels, X, featureNames] = ...
    local_load_training_data(dataDirectory)
features = readtable(fullfile(dataDirectory, 'navigation_features.csv'), ...
    TextType='string', VariableNamingRule='preserve');
labels = readtable(fullfile(dataDirectory, 'navigation_labels.csv'), ...
    TextType='string', VariableNamingRule='preserve');
exclusions = readtable(fullfile(dataDirectory, 'navigation_exclusions.csv'), ...
    TextType='string', VariableNamingRule='preserve');
local_validate_protected_data(labels, exclusions);
validLabel = labels.ground_truth == "SAFE" | labels.ground_truth == "DANGER";
validLabels = labels(validLabel, :);
[found, labelLocation] = ismember(features.content_sha256, ...
    validLabels.content_sha256);
trainingMask = features.complete & features.is_canonical & ...
    ~features.is_excluded & found;
training = features(trainingMask, :);
trainingLabels = validLabels.ground_truth(labelLocation(trainingMask));
firstFeature = find(strcmp(features.Properties.VariableNames, 'dark_all'), 1);
featureNames = string(features.Properties.VariableNames(firstFeature:end));
X = table2array(training(:, cellstr(featureNames)));
if any(~isfinite(X), 'all')
    error('Training features contain NaN or Inf values.');
end
end

function row = local_empty_curve_row()
row = struct('feature_count', 0, 'pooled_lambda_ratio', 0, ...
    'pooled_safe_permission_threshold', 0, 'pooled_missed_danger', 0, ...
    'pooled_false_danger', 0, 'nested_evaluated_count', 0, ...
    'nested_true_danger', 0, 'nested_missed_danger', 0, ...
    'nested_false_danger', 0, 'nested_true_safe', 0, ...
    'nested_danger_recall', 0, 'nested_safe_recall', 0, ...
    'nested_balanced_accuracy', 0);
end

function row = local_empty_fold_row()
row = struct('feature_count', 0, 'held_out_group', "", ...
    'test_count', 0, 'lambda_ratio', 0, ...
    'safe_permission_threshold', 0, 'true_danger', 0, ...
    'missed_danger', 0, 'false_danger', 0, 'true_safe', 0);
end

function row = local_empty_coefficient_row()
row = struct('feature_count', 0, 'rank', 0, 'feature_name', "", ...
    'standardization_mean', 0, 'standardization_scale', 0, ...
    'standardized_coefficient', 0, 'raw_coefficient', 0);
end

function local_validate_protected_data(labels, exclusions)
if height(exclusions) ~= 2 || ...
        ~any(exclusions.source_file == "两太阳能板带角度过度.txt" & ...
            exclusions.segment == 1) || ...
        ~any(exclusions.source_file == "太阳能板直角边源.txt" & ...
            exclusions.segment == 8)
    error('Protected navigation exclusions changed. No analysis was run.');
end
expectedLabels = struct('SAFE', 197, 'DANGER', 114, 'SEAM', 4, ...
    'EXCLUDED', 43, 'UNLABELED', 0);
names = fieldnames(expectedLabels);
for index = 1:numel(names)
    actual = nnz(labels.ground_truth == string(names{index}));
    if actual ~= expectedLabels.(names{index})
        error('Label guard failed for %s: expected %d, found %d.', ...
            names{index}, expectedLabels.(names{index}), actual);
    end
end
end
