function model = train_navigation_linear(dataDirectory, outputDirectory)
%TRAIN_NAVIGATION_LINEAR Group-validated regularized linear danger scorer.
% Toolbox-free MATLAB implementation. No Vehicle firmware is modified.

if nargin < 1 || strlength(string(dataDirectory)) == 0
    scriptDirectory = fileparts(mfilename('fullpath'));
    dataDirectory = fullfile(scriptDirectory, '..', 'data', 'navigation_20260816');
end
if nargin < 2 || strlength(string(outputDirectory)) == 0
    outputDirectory = fullfile(dataDirectory, 'model_linear');
end
if ~isfolder(outputDirectory)
    mkdir(outputDirectory);
end

config.danger_weight = 4.0;
config.threshold_step = 0.001;
config.reject_danger_threshold = 0.5;
config.coefficient_tolerance = 1.0e-7;
config.solver_max_iterations = 2000;
config.solver_tolerance = 1.0e-8;
config.run_nested_group_validation = true;
config.motion_classes = ["FORWARD", "REVERSE", "TURN_LEFT", ...
    "TURN_RIGHT", "STATIONARY"];

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
y = double(trainingLabels == "DANGER");

firstFeature = find(strcmp(features.Properties.VariableNames, 'dark_all'), 1);
imageFeatureNames = string(features.Properties.VariableNames(firstFeature:end));
imageX = table2array(training(:, cellstr(imageFeatureNames)));
if any(~isfinite(imageX), 'all')
    error('Training features contain NaN or Inf values.');
end

motionContext = validLabels.motion_context(labelLocation(trainingMask));
motionX = zeros(height(training), numel(config.motion_classes));
for motionIndex = 1:numel(config.motion_classes)
    motionX(:, motionIndex) = ...
        double(motionContext == config.motion_classes(motionIndex));
end
knownMotion = ismember(motionContext, config.motion_classes);
fprintf('Motion known: %d/%d (%.1f%%); UNKNOWN remains all-zero.\n', ...
    nnz(knownMotion), height(training), 100 * mean(knownMotion));

captureGroup = training.source_file + "|" + string(training.segment);
groups = unique(captureGroup, 'stable');
candidates = local_candidate_table();
candidateCount = height(candidates);
sampleCount = height(training);
oofScore = nan(sampleCount, candidateCount);

for candidateIndex = 1:candidateCount
    fprintf('Validating candidate %d/%d: %s\n', candidateIndex, ...
        candidateCount, candidates.name(candidateIndex));
    for heldOutGroup = groups.'
        testMask = captureGroup == heldOutGroup;
        trainMask = ~testMask;
        if numel(unique(y(trainMask))) < 2
            continue;
        end
        design = local_prepare_design(imageX(trainMask, :), ...
            imageX(testMask, :), motionX(trainMask, :), ...
            motionX(testMask, :), imageFeatureNames, ...
            config.motion_classes, candidates.include_motion(candidateIndex), ...
            candidates.interactions(candidateIndex));
        fit = local_fit_logistic(design.trainX, y(trainMask), ...
            candidates.alpha(candidateIndex), ...
            candidates.lambda_ratio(candidateIndex), config);
        oofScore(testMask, candidateIndex) = ...
            local_sigmoid(fit.intercept + design.testX * fit.coefficients);
    end
end

candidateRows = repmat(local_empty_candidate_result(), candidateCount, 1);
candidateSweeps = cell(candidateCount, 1);
for candidateIndex = 1:candidateCount
    evaluated = isfinite(oofScore(:, candidateIndex));
    sweep = local_threshold_sweep(y(evaluated) > 0, ...
        oofScore(evaluated, candidateIndex), config.threshold_step);
    bestIndex = local_best_threshold_index(sweep);
    candidateSweeps{candidateIndex} = sweep;
    row = local_empty_candidate_result();
    row.name = candidates.name(candidateIndex);
    row.include_motion = candidates.include_motion(candidateIndex);
    row.interactions = candidates.interactions(candidateIndex);
    row.alpha = candidates.alpha(candidateIndex);
    row.lambda_ratio = candidates.lambda_ratio(candidateIndex);
    row.safe_permission_threshold = sweep.threshold(bestIndex);
    row.evaluated_count = sweep.evaluated_count(bestIndex);
    row.true_danger = sweep.true_danger(bestIndex);
    row.missed_danger = sweep.missed_danger(bestIndex);
    row.false_danger = sweep.false_danger(bestIndex);
    row.true_safe = sweep.true_safe(bestIndex);
    row.danger_recall = sweep.danger_recall(bestIndex);
    row.safe_recall = sweep.safe_recall(bestIndex);
    row.balanced_accuracy = sweep.balanced_accuracy(bestIndex);
    fullCandidateDesign = local_prepare_design(imageX, imageX, motionX, ...
        motionX, imageFeatureNames, config.motion_classes, ...
        candidates.include_motion(candidateIndex), ...
        candidates.interactions(candidateIndex));
    fullCandidateFit = local_fit_logistic(fullCandidateDesign.trainX, y, ...
        candidates.alpha(candidateIndex), ...
        candidates.lambda_ratio(candidateIndex), config);
    row.active_coefficient_count = nnz(abs(fullCandidateFit.coefficients) > ...
        config.coefficient_tolerance);
    row.total_coefficient_count = numel(fullCandidateFit.coefficients);
    candidateRows(candidateIndex) = row;
end
candidateResults = struct2table(candidateRows);
bestCandidate = local_best_candidate_index(candidateResults);
bestSweep = candidateSweeps{bestCandidate};
bestThresholdIndex = local_best_threshold_index(bestSweep);
safePermissionThreshold = bestSweep.threshold(bestThresholdIndex);
dangerThreshold = max(config.reject_danger_threshold, ...
    safePermissionThreshold);

fullDesign = local_prepare_design(imageX, imageX, motionX, motionX, ...
    imageFeatureNames, config.motion_classes, ...
    candidates.include_motion(bestCandidate), ...
    candidates.interactions(bestCandidate));
fit = local_fit_logistic(fullDesign.trainX, y, ...
    candidates.alpha(bestCandidate), ...
    candidates.lambda_ratio(bestCandidate), config);
fullScore = local_sigmoid(fit.intercept + ...
    fullDesign.testX * fit.coefficients);

bestOofScore = oofScore(:, bestCandidate);
validationPrediction = repmat("NOT_EVALUATED", sampleCount, 1);
evaluated = isfinite(bestOofScore);
validationPrediction(evaluated & bestOofScore < safePermissionThreshold) = "SAFE";
validationPrediction(evaluated & bestOofScore >= safePermissionThreshold & ...
    bestOofScore < dangerThreshold) = "UNKNOWN";
validationPrediction(evaluated & bestOofScore >= dangerThreshold) = "DANGER";

nested = local_nested_group_validation(imageX, motionX, y, captureGroup, ...
    imageFeatureNames, candidates, config);
nestedMetrics = local_calculate_metrics(y(nested.evaluated) > 0, ...
    nested.stopRequired(nested.evaluated));

validation = training(:, {'content_sha256', 'source_file', 'scenario', ...
    'segment', 'begin_frame'});
validation.ground_truth = trainingLabels;
validation.motion_context = motionContext;
validation.training_full_model_danger_score = fullScore;
validation.group_validation_danger_score = bestOofScore;
validation.group_validation_prediction = validationPrediction;
validation.stop_required = validationPrediction == "DANGER" | ...
    validationPrediction == "UNKNOWN";
validation.nested_danger_score = nested.score;
validation.nested_safe_permission_threshold = nested.threshold;
validation.nested_selected_candidate = nested.candidate;
validation.nested_stop_required = nested.stopRequired;

coefficientTable = table(fullDesign.featureNames.', ...
    fit.coefficients, abs(fit.coefficients), ...
    VariableNames={'feature_name', 'coefficient', 'absolute_coefficient'});
coefficientTable = sortrows(coefficientTable, ...
    'absolute_coefficient', 'descend');
active = coefficientTable.absolute_coefficient > ...
    config.coefficient_tolerance;

model.config = config;
model.family = "REGULARIZED_LOGISTIC_LINEAR_SCORER";
model.candidate = candidates(bestCandidate, :);
model.image_feature_names = imageFeatureNames;
model.motion_classes = config.motion_classes;
model.motion_unknown_encoding = "ALL_ZERO";
model.feature_names = fullDesign.featureNames;
model.standardization_mean = fullDesign.mean;
model.standardization_scale = fullDesign.scale;
model.intercept = fit.intercept;
model.coefficients = fit.coefficients;
model.lambda = fit.lambda;
model.safe_permission_threshold = safePermissionThreshold;
model.danger_threshold = dangerThreshold;
model.reject_semantics = "SAFE below safe threshold; UNKNOWN and DANGER STOP";
model.nonzero_coefficient_count = nnz(active);
model.nested_group_validation_metrics = nestedMetrics;
if nestedMetrics.missed_danger == 0
    model.deployment_recommendation = "OFFLINE_CANDIDATE_REQUIRES_FIXED_POINT_VALIDATION";
else
    model.deployment_recommendation = "DO_NOT_DEPLOY";
end
save(fullfile(outputDirectory, 'navigation_linear_model.mat'), 'model');

writetable(candidateResults, fullfile(outputDirectory, ...
    'navigation_linear_candidates.csv'));
writetable(bestSweep, fullfile(outputDirectory, ...
    'navigation_linear_threshold_sweep.csv'));
writetable(validation, fullfile(outputDirectory, ...
    'navigation_linear_validation.csv'));
writetable(validation(evaluated & validation.stop_required ~= (y > 0), :), ...
    fullfile(outputDirectory, 'navigation_linear_misclassified.csv'));
writetable(coefficientTable, fullfile(outputDirectory, ...
    'navigation_linear_coefficients.csv'));
writetable(nested.foldTable, fullfile(outputDirectory, ...
    'navigation_linear_nested_folds.csv'));
writetable(struct2table(nestedMetrics), fullfile(outputDirectory, ...
    'navigation_linear_nested_metrics.csv'));

best = candidateResults(bestCandidate, :);
fprintf(['Selected %s: threshold=%.3f TD=%d MD=%d FD=%d TS=%d ' ...
    'danger_recall=%.3f safe_recall=%.3f balanced_accuracy=%.3f.\n'], ...
    best.name, safePermissionThreshold, best.true_danger, ...
    best.missed_danger, best.false_danger, best.true_safe, ...
    best.danger_recall, best.safe_recall, best.balanced_accuracy);
fprintf(['Reject policy: SAFE score < %.3f, UNKNOWN %.3f <= score < %.3f, ' ...
    'DANGER score >= %.3f. UNKNOWN executes STOP.\n'], ...
    safePermissionThreshold, safePermissionThreshold, dangerThreshold, ...
    dangerThreshold);
fprintf('Full model active coefficients: %d/%d. Output: %s\n', ...
    nnz(active), height(coefficientTable), outputDirectory);
fprintf(['Nested group validation: TD=%d MD=%d FD=%d TS=%d ' ...
    'danger_recall=%.3f safe_recall=%.3f balanced_accuracy=%.3f.\n'], ...
    nestedMetrics.true_danger, nestedMetrics.missed_danger, ...
    nestedMetrics.false_danger, nestedMetrics.true_safe, ...
    nestedMetrics.danger_recall, nestedMetrics.safe_recall, ...
    nestedMetrics.balanced_accuracy);
end

function nested = local_nested_group_validation(imageX, motionX, y, ...
    captureGroup, imageFeatureNames, candidates, config)
sampleCount = numel(y);
nested.score = nan(sampleCount, 1);
nested.threshold = nan(sampleCount, 1);
nested.candidate = repmat("NOT_EVALUATED", sampleCount, 1);
nested.stopRequired = false(sampleCount, 1);
groups = unique(captureGroup, 'stable');
foldRows = repmat(struct('held_out_group', "", 'test_count', 0, ...
    'selected_candidate', "", 'safe_permission_threshold', NaN, ...
    'true_danger', 0, 'missed_danger', 0, 'false_danger', 0, ...
    'true_safe', 0), numel(groups), 1);
if ~config.run_nested_group_validation
    nested.evaluated = false(sampleCount, 1);
    nested.foldTable = struct2table(foldRows([]));
    return;
end

for outerIndex = 1:numel(groups)
    heldOutGroup = groups(outerIndex);
    fprintf('Nested validation outer group %d/%d: %s\n', ...
        outerIndex, numel(groups), heldOutGroup);
    outerTest = captureGroup == heldOutGroup;
    outerTrain = ~outerTest;
    innerGroups = unique(captureGroup(outerTrain), 'stable');
    innerScore = nan(nnz(outerTrain), height(candidates));
    outerTrainIndices = find(outerTrain);
    for candidateIndex = 1:height(candidates)
        for innerHeldOut = innerGroups.'
            innerTestGlobal = outerTrain & captureGroup == innerHeldOut;
            innerFitGlobal = outerTrain & captureGroup ~= innerHeldOut;
            if numel(unique(y(innerFitGlobal))) < 2
                continue;
            end
            design = local_prepare_design(imageX(innerFitGlobal, :), ...
                imageX(innerTestGlobal, :), motionX(innerFitGlobal, :), ...
                motionX(innerTestGlobal, :), imageFeatureNames, ...
                config.motion_classes, ...
                candidates.include_motion(candidateIndex), ...
                candidates.interactions(candidateIndex));
            fit = local_fit_logistic(design.trainX, y(innerFitGlobal), ...
                candidates.alpha(candidateIndex), ...
                candidates.lambda_ratio(candidateIndex), config);
            innerLocalRows = ismember(outerTrainIndices, find(innerTestGlobal));
            innerScore(innerLocalRows, candidateIndex) = local_sigmoid( ...
                fit.intercept + design.testX * fit.coefficients);
        end
    end

    innerResults = repmat(local_empty_candidate_result(), height(candidates), 1);
    innerSweeps = cell(height(candidates), 1);
    innerY = y(outerTrain) > 0;
    for candidateIndex = 1:height(candidates)
        innerEvaluated = isfinite(innerScore(:, candidateIndex));
        sweep = local_threshold_sweep(innerY(innerEvaluated), ...
            innerScore(innerEvaluated, candidateIndex), config.threshold_step);
        thresholdIndex = local_best_threshold_index(sweep);
        innerSweeps{candidateIndex} = sweep;
        row = local_empty_candidate_result();
        row.name = candidates.name(candidateIndex);
        row.missed_danger = sweep.missed_danger(thresholdIndex);
        row.false_danger = sweep.false_danger(thresholdIndex);
        row.balanced_accuracy = sweep.balanced_accuracy(thresholdIndex);
        innerResults(candidateIndex) = row;
    end
    innerResults = struct2table(innerResults);
    selectedIndex = local_best_candidate_index(innerResults);
    selectedSweep = innerSweeps{selectedIndex};
    selectedThresholdIndex = local_best_threshold_index(selectedSweep);
    selectedThreshold = selectedSweep.threshold(selectedThresholdIndex);

    design = local_prepare_design(imageX(outerTrain, :), ...
        imageX(outerTest, :), motionX(outerTrain, :), motionX(outerTest, :), ...
        imageFeatureNames, config.motion_classes, ...
        candidates.include_motion(selectedIndex), ...
        candidates.interactions(selectedIndex));
    fit = local_fit_logistic(design.trainX, y(outerTrain), ...
        candidates.alpha(selectedIndex), ...
        candidates.lambda_ratio(selectedIndex), config);
    score = local_sigmoid(fit.intercept + design.testX * fit.coefficients);
    stopRequired = score >= selectedThreshold;
    nested.score(outerTest) = score;
    nested.threshold(outerTest) = selectedThreshold;
    nested.candidate(outerTest) = candidates.name(selectedIndex);
    nested.stopRequired(outerTest) = stopRequired;

    metrics = local_calculate_metrics(y(outerTest) > 0, stopRequired);
    foldRows(outerIndex).held_out_group = heldOutGroup;
    foldRows(outerIndex).test_count = nnz(outerTest);
    foldRows(outerIndex).selected_candidate = candidates.name(selectedIndex);
    foldRows(outerIndex).safe_permission_threshold = selectedThreshold;
    foldRows(outerIndex).true_danger = metrics.true_danger;
    foldRows(outerIndex).missed_danger = metrics.missed_danger;
    foldRows(outerIndex).false_danger = metrics.false_danger;
    foldRows(outerIndex).true_safe = metrics.true_safe;
end
nested.evaluated = isfinite(nested.score) & isfinite(nested.threshold);
nested.foldTable = struct2table(foldRows);
end

function candidates = local_candidate_table()
name = ["IMAGE_RIDGE_0.03"; "IMAGE_RIDGE_0.10"; "IMAGE_RIDGE_0.30"; ...
    "MOTION_RIDGE_0.03"; "MOTION_RIDGE_0.10"; "MOTION_RIDGE_0.30"; ...
    "IMAGE_ELASTIC_0.03"; "IMAGE_ELASTIC_0.10"; ...
    "IMAGE_ELASTIC_0.30"; "MOTION_ELASTIC_0.03"; ...
    "MOTION_ELASTIC_0.10"; "MOTION_ELASTIC_0.30"; ...
    "INTERACTION_ELASTIC_0.01"; "INTERACTION_ELASTIC_0.03"; ...
    "INTERACTION_ELASTIC_0.10"; "INTERACTION_ELASTIC_0.30"];
include_motion = [false(3, 1); true(3, 1); false(3, 1); ...
    true(7, 1)];
interactions = [false(12, 1); true(4, 1)];
alpha = [zeros(6, 1); 0.8 * ones(10, 1)];
lambda_ratio = [0.03; 0.10; 0.30; 0.03; 0.10; 0.30; ...
    0.03; 0.10; 0.30; 0.03; 0.10; 0.30; ...
    0.01; 0.03; 0.10; 0.30];
candidates = table(name, include_motion, interactions, alpha, lambda_ratio);
end

function design = local_prepare_design(trainImage, testImage, trainMotion, ...
    testMotion, imageNames, motionClasses, includeMotion, interactions)
meanValue = mean(trainImage, 1);
scaleValue = std(trainImage, 0, 1);
scaleValue(~isfinite(scaleValue) | scaleValue < 1.0e-12) = 1;
trainImageZ = (trainImage - meanValue) ./ scaleValue;
testImageZ = (testImage - meanValue) ./ scaleValue;
trainX = trainImageZ;
testX = testImageZ;
featureNames = imageNames;
if includeMotion
    trainX = [trainX, trainMotion];
    testX = [testX, testMotion];
    featureNames = [featureNames, "motion_" + motionClasses];
end
if interactions
    for motionIndex = 1:numel(motionClasses)
        trainX = [trainX, trainImageZ .* trainMotion(:, motionIndex)]; %#ok<AGROW>
        testX = [testX, testImageZ .* testMotion(:, motionIndex)]; %#ok<AGROW>
        featureNames = [featureNames, imageNames + "__motion_" + ...
            motionClasses(motionIndex)]; %#ok<AGROW>
    end
end
design.trainX = trainX;
design.testX = testX;
design.featureNames = featureNames;
design.mean = meanValue;
design.scale = scaleValue;
end

function fit = local_fit_logistic(X, y, alpha, lambdaRatio, config)
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
step = 1 / (0.25 * spectralSquared / weightSum + ...
    lambda * (1 - alpha) + 1.0e-12);

beta = [intercept; zeros(size(X, 2), 1)];
momentumPoint = beta;
momentum = 1;
for iteration = 1:config.solver_max_iterations
    probability = local_sigmoid(augmented * momentumPoint);
    gradient = augmented.' * (sampleWeight .* (probability - y)) / weightSum;
    gradient(2:end) = gradient(2:end) + ...
        lambda * (1 - alpha) * momentumPoint(2:end);
    next = momentumPoint - step * gradient;
    next(2:end) = local_soft_threshold(next(2:end), ...
        step * lambda * alpha);
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

function value = local_soft_threshold(value, threshold)
value = sign(value) .* max(abs(value) - threshold, 0);
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
    trueDanger = nnz(actualDanger & predictedDanger);
    missedDanger = nnz(actualDanger & ~predictedDanger);
    falseDanger = nnz(~actualDanger & predictedDanger);
    trueSafe = nnz(~actualDanger & ~predictedDanger);
    dangerRecall = trueDanger / max(1, trueDanger + missedDanger);
    safeRecall = trueSafe / max(1, trueSafe + falseDanger);
    sweep{index, 2:end} = [numel(actualDanger), trueDanger, missedDanger, ...
        falseDanger, trueSafe, dangerRecall, safeRecall, ...
        (dangerRecall + safeRecall) / 2, ...
        min(abs(probability - thresholds(index)))];
end
end

function bestIndex = local_best_threshold_index(sweep)
candidates = find(sweep.missed_danger == min(sweep.missed_danger));
candidates = candidates(sweep.false_danger(candidates) == ...
    min(sweep.false_danger(candidates)));
[~, relativeIndex] = max(sweep.balanced_accuracy(candidates));
candidates = candidates(sweep.balanced_accuracy(candidates) == ...
    sweep.balanced_accuracy(candidates(relativeIndex)));
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

function bestIndex = local_best_candidate_index(results)
candidates = find(results.missed_danger == min(results.missed_danger));
candidates = candidates(results.false_danger(candidates) == ...
    min(results.false_danger(candidates)));
[~, relativeIndex] = max(results.balanced_accuracy(candidates));
bestIndex = candidates(relativeIndex);
end

function row = local_empty_candidate_result()
row = struct('name', "", 'include_motion', false, ...
    'interactions', false, 'alpha', 0, ...
    'lambda_ratio', 0, 'safe_permission_threshold', 0, ...
    'evaluated_count', 0, 'true_danger', 0, 'missed_danger', 0, ...
    'false_danger', 0, 'true_safe', 0, 'danger_recall', 0, ...
    'safe_recall', 0, 'balanced_accuracy', 0, ...
    'active_coefficient_count', 0, 'total_coefficient_count', 0);
end

function local_validate_protected_data(labels, exclusions)
if height(exclusions) ~= 2 || ...
        ~any(exclusions.source_file == "两太阳能板带角度过度.txt" & ...
            exclusions.segment == 1) || ...
        ~any(exclusions.source_file == "太阳能板直角边源.txt" & ...
            exclusions.segment == 8)
    error(['Protected navigation_exclusions.csv does not contain exactly ' ...
        'the two expected exclusions. No training was performed.']);
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
