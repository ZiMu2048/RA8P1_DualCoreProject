function export_navigation_c_test_vectors(dataDirectory, fixedModelPath, outputPath)
%EXPORT_NAVIGATION_C_TEST_VECTORS Write images and exact integer references.

if nargin < 1 || strlength(string(dataDirectory)) == 0
    scriptDirectory = fileparts(mfilename('fullpath'));
    dataDirectory = fullfile(scriptDirectory, '..', 'data', 'navigation_20260816');
end
if nargin < 2 || strlength(string(fixedModelPath)) == 0
    fixedModelPath = fullfile(dataDirectory, 'model_fixed_mcu_simple', ...
        'navigation_fixed_point_models.mat');
end
if nargin < 3 || strlength(string(outputPath)) == 0
    outputPath = fullfile(dataDirectory, 'model_fixed_mcu_simple', ...
        'navigation_c_test_vectors.bin');
end

loaded = load(fixedModelPath);
model = loaded.fixedPoint.strict_model;
if model.feature_count ~= 24 || model.input_fraction_bits ~= 4
    error('Expected the 24-feature F4 MCU-simple model.');
end

features = readtable(fullfile(dataDirectory, 'navigation_features.csv'), ...
    TextType='string', VariableNamingRule='preserve');
labels = readtable(fullfile(dataDirectory, 'navigation_labels.csv'), ...
    TextType='string', VariableNamingRule='preserve');
rowsPath = fullfile(dataDirectory, 'binary_rows.csv');
options = detectImportOptions(rowsPath, TextType='string', ...
    VariableNamingRule='preserve');
options = setvartype(options, 'bits', 'string');
rows = readtable(rowsPath, options);

validLabel = labels.ground_truth == "SAFE" | labels.ground_truth == "DANGER";
validLabels = labels(validLabel, :);
[found, labelLocation] = ismember(features.content_sha256, ...
    validLabels.content_sha256);
mask = features.complete & features.is_canonical & ...
    ~features.is_excluded & found;
selected = features(mask, :);
groundTruth = validLabels.ground_truth(labelLocation(mask));
if height(selected) ~= 311 || nnz(groundTruth == "DANGER") ~= 114
    error('Test-vector data guard failed.');
end

file = fopen(outputPath, 'w', 'ieee-le');
if file < 0
    error('Cannot open %s for writing.', outputPath);
end
cleanup = onCleanup(@() fclose(file));
header = uint32([hex2dec('4E415654'), 1, height(selected), 200, 24, 24]);
fwrite(file, header, 'uint32');

for sample = 1:height(selected)
    selectedRows = rows(rows.snapshot_id == selected.snapshot_id(sample), :);
    [~, order] = sort(selectedRows.row_index);
    selectedRows = selectedRows(order, :);
    if height(selectedRows) ~= 24
        error('Snapshot %s does not have 24 rows.', selected.snapshot_id(sample));
    end
    dark = char(selectedRows.bits) == '1';
    if ~isequal(size(dark), [24, 200])
        error('Snapshot %s is not 24x200.', selected.snapshot_id(sample));
    end
    featureValues = table2array(selected(sample, cellstr(model.feature_names)));
    featureQ = int32(round(featureValues * 2^model.input_fraction_bits));
    scoreQ = model.quantized_intercept;
    for feature = 1:model.feature_count
        scoreQ = scoreQ + int64(featureQ(feature)) * ...
            model.quantized_coefficients(feature);
    end
    stopRequired = uint8(scoreQ >= model.quantized_logit_threshold);
    fwrite(file, reshape(uint8(dark).', [], 1), 'uint8');
    fwrite(file, featureQ, 'int32');
    fwrite(file, scoreQ, 'int64');
    fwrite(file, stopRequired, 'uint8');
end
fprintf('C test vectors: %d samples written to %s\n', height(selected), outputPath);
end
