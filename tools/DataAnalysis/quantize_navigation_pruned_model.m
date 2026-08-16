function fixedPoint = quantize_navigation_pruned_model(dataDirectory, ...
    pruningModelPath, outputDirectory)
%QUANTIZE_NAVIGATION_PRUNED_MODEL Simulate integer-only danger scoring.
% This writes offline reference files only and does not modify firmware.

if nargin < 1 || strlength(string(dataDirectory)) == 0
    scriptDirectory = fileparts(mfilename('fullpath'));
    dataDirectory = fullfile(scriptDirectory, '..', 'data', 'navigation_20260816');
end
if nargin < 2 || strlength(string(pruningModelPath)) == 0
    pruningModelPath = fullfile(dataDirectory, 'model_pruned_spatial', ...
        'navigation_pruning_result.mat');
end
if nargin < 3 || strlength(string(outputDirectory)) == 0
    outputDirectory = fullfile(dataDirectory, 'model_fixed');
end
if ~isfolder(outputDirectory)
    mkdir(outputDirectory);
end

loaded = load(pruningModelPath);
if ~isfield(loaded, 'result')
    error('Pruning MAT file does not contain result.');
end
pruning = loaded.result;
if ~ismember(pruning.strict_best_model.config.feature_mode, ...
        ["spatial", "mcu_simple"])
    error('Only spatial or mcu_simple models may enter fixed-point analysis.');
end

[metadata, groundTruth, features] = local_load_labeled_features(dataDirectory);
models = {pruning.strict_best_model, pruning.smallest_zero_miss_model};
modelNames = ["STRICT", "COMPACT"];
inputFractionCandidates = [4, 6, 8, 10];
coefficientFractionCandidates = [14, 16, 18, 20, 22];
sweepRows = repmat(local_empty_sweep_row(), 0, 1);
coefficientRows = repmat(local_empty_coefficient_row(), 0, 1);
validation = metadata;
validation.ground_truth = groundTruth;
exportedModels = cell(numel(models), 1);

for modelIndex = 1:numel(models)
    model = models{modelIndex};
    name = modelNames(modelIndex);
    X = table2array(features(:, cellstr(model.feature_names)));
    floatLogit = model.raw_intercept + X * model.raw_coefficients;
    floatStop = floatLogit >= model.safe_permission_logit_threshold;
    floatMargin = abs(floatLogit - model.safe_permission_logit_threshold);
    minimumFloatMargin = min(floatMargin);

    modelSweepRows = repmat(local_empty_sweep_row(), 0, 1);
    for inputFractionBits = inputFractionCandidates
        for coefficientFractionBits = coefficientFractionCandidates
            simulation = local_simulate(X, model, inputFractionBits, ...
                coefficientFractionBits);
            row = local_empty_sweep_row();
            row.model_name = name;
            row.feature_count = model.feature_count;
            row.input_fraction_bits = inputFractionBits;
            row.coefficient_fraction_bits = coefficientFractionBits;
            row.accumulator_fraction_bits = ...
                inputFractionBits + coefficientFractionBits;
            row.decision_flips = nnz(simulation.stopRequired ~= floatStop);
            row.danger_to_safe_flips = nnz(groundTruth == "DANGER" & ...
                floatStop & ~simulation.stopRequired);
            row.fixed_missed_danger = nnz(groundTruth == "DANGER" & ...
                ~simulation.stopRequired);
            row.fixed_false_danger = nnz(groundTruth == "SAFE" & ...
                simulation.stopRequired);
            row.max_logit_error = max(abs(simulation.logit - floatLogit));
            row.mean_logit_error = mean(abs(simulation.logit - floatLogit));
            row.minimum_float_margin = minimumFloatMargin;
            row.minimum_fixed_margin = min(abs(simulation.logit - ...
                model.safe_permission_logit_threshold));
            row.max_abs_accumulator = max(abs(double(simulation.accumulator)));
            row.int64_headroom_bits = local_headroom_bits( ...
                row.max_abs_accumulator);
            modelSweepRows(end + 1) = row; %#ok<AGROW>
            sweepRows(end + 1) = row; %#ok<AGROW>
        end
    end

    modelSweep = struct2table(modelSweepRows);
    recommendedIndex = local_recommended_format(modelSweep);
    recommended = modelSweep(recommendedIndex, :);
    simulation = local_simulate(X, model, ...
        recommended.input_fraction_bits, ...
        recommended.coefficient_fraction_bits);
    suffix = lower(name);
    validation.("float_logit_" + suffix) = floatLogit;
    validation.("fixed_logit_" + suffix) = simulation.logit;
    validation.("fixed_error_" + suffix) = simulation.logit - floatLogit;
    validation.("float_stop_" + suffix) = floatStop;
    validation.("fixed_stop_" + suffix) = simulation.stopRequired;

    fixedModel = model;
    fixedModel.model_name = name;
    fixedModel.input_fraction_bits = recommended.input_fraction_bits;
    fixedModel.coefficient_fraction_bits = ...
        recommended.coefficient_fraction_bits;
    fixedModel.accumulator_fraction_bits = ...
        recommended.accumulator_fraction_bits;
    fixedModel.quantized_coefficients = simulation.coefficients;
    fixedModel.quantized_intercept = simulation.intercept;
    fixedModel.quantized_logit_threshold = simulation.threshold;
    fixedModel.quantization_metrics = table2struct(recommended);
    fixedModel.integer_decision = ...
        "ALLOW iff intercept_q + sum(feature_q * coefficient_q) < threshold_q";
    exportedModels{modelIndex} = fixedModel;

    for featureIndex = 1:model.feature_count
        coefficientRow = local_empty_coefficient_row();
        coefficientRow.model_name = name;
        coefficientRow.feature_index = featureIndex - 1;
        coefficientRow.feature_name = model.feature_names(featureIndex);
        coefficientRow.raw_coefficient = model.raw_coefficients(featureIndex);
        coefficientRow.quantized_coefficient = ...
            simulation.coefficients(featureIndex);
        coefficientRows(end + 1) = coefficientRow; %#ok<AGROW>
    end

    fprintf(['%s K=%d: input F%d, coefficient F%d, flips=%d, ' ...
        'danger-to-safe=%d, max_error=%.3g, int64_headroom=%d bits.\n'], ...
        name, model.feature_count, recommended.input_fraction_bits, ...
        recommended.coefficient_fraction_bits, recommended.decision_flips, ...
        recommended.danger_to_safe_flips, recommended.max_logit_error, ...
        recommended.int64_headroom_bits);
end

fixedPoint.source_pruning_model = string(pruningModelPath);
fixedPoint.strict_model = exportedModels{1};
fixedPoint.compact_model = exportedModels{2};
fixedPoint.deployment_recommendation = ...
    "OFFLINE_FIXED_POINT_EQUIVALENT_AWAIT_FEATURE_EXTRACTOR_C_VALIDATION";
save(fullfile(outputDirectory, 'navigation_fixed_point_models.mat'), ...
    'fixedPoint');
writetable(struct2table(sweepRows), fullfile(outputDirectory, ...
    'navigation_fixed_point_sweep.csv'));
writetable(struct2table(coefficientRows), fullfile(outputDirectory, ...
    'navigation_fixed_point_coefficients.csv'));
writetable(validation, fullfile(outputDirectory, ...
    'navigation_fixed_point_validation.csv'));
fprintf('No firmware was modified. Output: %s\n', outputDirectory);
end

function simulation = local_simulate(X, model, inputFractionBits, ...
    coefficientFractionBits)
inputScale = 2^inputFractionBits;
coefficientScale = 2^coefficientFractionBits;
accumulatorScale = inputScale * coefficientScale;
inputQ = int64(round(X * inputScale));
coefficientQ = int64(round(model.raw_coefficients * coefficientScale));
interceptQ = int64(round(model.raw_intercept * accumulatorScale));
thresholdQ = int64(round(model.safe_permission_logit_threshold * ...
    accumulatorScale));
accumulator = repmat(interceptQ, size(X, 1), 1);
for row = 1:size(X, 1)
    accumulator(row) = accumulator(row) + ...
        sum(inputQ(row, :) .* coefficientQ.');
end
simulation.logit = double(accumulator) / accumulatorScale;
simulation.stopRequired = accumulator >= thresholdQ;
simulation.accumulator = accumulator;
simulation.coefficients = coefficientQ;
simulation.intercept = interceptQ;
simulation.threshold = thresholdQ;
end

function index = local_recommended_format(sweep)
acceptable = find(sweep.decision_flips == 0 & ...
    sweep.danger_to_safe_flips == 0 & ...
    sweep.max_logit_error <= 0.25 * sweep.minimum_float_margin);
if isempty(acceptable)
    acceptable = find(sweep.decision_flips == 0 & ...
        sweep.danger_to_safe_flips == 0);
end
if isempty(acceptable)
    candidates = (1:height(sweep)).';
    candidates = candidates(sweep.danger_to_safe_flips(candidates) == ...
        min(sweep.danger_to_safe_flips(candidates)));
    candidates = candidates(sweep.decision_flips(candidates) == ...
        min(sweep.decision_flips(candidates)));
    [~, relativeIndex] = min(sweep.max_logit_error(candidates));
    index = candidates(relativeIndex);
    return;
end
minimumFractionBits = min(sweep.accumulator_fraction_bits(acceptable));
acceptable = acceptable(sweep.accumulator_fraction_bits(acceptable) == ...
    minimumFractionBits);
[~, relativeIndex] = max(sweep.minimum_fixed_margin(acceptable));
index = acceptable(relativeIndex);
end

function bits = local_headroom_bits(maximumValue)
if maximumValue <= 0
    bits = 63;
else
    bits = 62 - ceil(log2(maximumValue));
end
end

function [metadata, groundTruth, featureTable] = ...
    local_load_labeled_features(dataDirectory)
features = readtable(fullfile(dataDirectory, 'navigation_features.csv'), ...
    TextType='string', VariableNamingRule='preserve');
labels = readtable(fullfile(dataDirectory, 'navigation_labels.csv'), ...
    TextType='string', VariableNamingRule='preserve');
validLabel = labels.ground_truth == "SAFE" | labels.ground_truth == "DANGER";
validLabels = labels(validLabel, :);
[found, labelLocation] = ismember(features.content_sha256, ...
    validLabels.content_sha256);
mask = features.complete & features.is_canonical & ...
    ~features.is_excluded & found;
selected = features(mask, :);
groundTruth = validLabels.ground_truth(labelLocation(mask));
metadata = selected(:, {'content_sha256', 'source_file', 'scenario', ...
    'segment', 'begin_frame'});
featureTable = selected;
if height(featureTable) ~= 311 || nnz(groundTruth == "DANGER") ~= 114
    error('Training data guard failed before quantization.');
end
end

function row = local_empty_sweep_row()
row = struct('model_name', "", 'feature_count', 0, ...
    'input_fraction_bits', 0, 'coefficient_fraction_bits', 0, ...
    'accumulator_fraction_bits', 0, 'decision_flips', 0, ...
    'danger_to_safe_flips', 0, 'fixed_missed_danger', 0, ...
    'fixed_false_danger', 0, 'max_logit_error', 0, ...
    'mean_logit_error', 0, 'minimum_float_margin', 0, ...
    'minimum_fixed_margin', 0, 'max_abs_accumulator', 0, ...
    'int64_headroom_bits', 0);
end

function row = local_empty_coefficient_row()
row = struct('model_name', "", 'feature_index', 0, ...
    'feature_name', "", 'raw_coefficient', 0, ...
    'quantized_coefficient', int64(0));
end
