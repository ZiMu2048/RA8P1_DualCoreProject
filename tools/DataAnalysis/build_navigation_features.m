function build_navigation_features(dataDirectory)
%BUILD_NAVIGATION_FEATURES Convert every normalized snapshot to ML features.
% Incomplete snapshots remain in the output with NaN image features.

if nargin < 1 || strlength(string(dataDirectory)) == 0
    scriptDirectory = fileparts(mfilename('fullpath'));
    dataDirectory = fullfile(scriptDirectory, '..', 'data', 'navigation_20260816');
end

snapshots = readtable(fullfile(dataDirectory, 'binary_snapshots.csv'), ...
    TextType='string', VariableNamingRule='preserve');
binaryRowsPath = fullfile(dataDirectory, 'binary_rows.csv');
binaryRowsOptions = detectImportOptions(binaryRowsPath, ...
    TextType='string', VariableNamingRule='preserve');
binaryRowsOptions = setvartype(binaryRowsOptions, 'bits', 'string');
rows = readtable(binaryRowsPath, binaryRowsOptions);

completeMask = local_logical_column(snapshots.complete);
snapshotCount = height(snapshots);
[isExcluded, exclusionReason] = local_exclusion_mask(snapshots, dataDirectory);
[featureNames, gridNames, temporalNames] = local_feature_names();
spatialFeatureNames = [featureNames, gridNames];
allFeatureNames = [spatialFeatureNames, temporalNames];
featureValues = nan(snapshotCount, numel(allFeatureNames));

for snapshotIndex = find(completeMask).'
    selected = rows(rows.snapshot_id == snapshots.snapshot_id(snapshotIndex), :);
    [~, order] = sort(selected.row_index);
    selected = selected(order, :);
    if height(selected) ~= 24
        error('Complete snapshot %s does not contain 24 rows.', ...
            snapshots.snapshot_id(snapshotIndex));
    end
    bits = char(selected.bits);
    if size(bits, 2) ~= 200
        error('Complete snapshot %s does not have width 200.', ...
            snapshots.snapshot_id(snapshotIndex));
    end
    featureValues(snapshotIndex, 1:numel(spatialFeatureNames)) = ...
        local_image_features(bits == '1');
end
featureValues = local_temporal_features(snapshots, completeMask, ...
    featureValues, spatialFeatureNames, temporalNames);

hashes = string(snapshots.content_sha256);
duplicateCount = zeros(snapshotCount, 1);
for group = unique(hashes).'
    selected = hashes == group;
    duplicateCount(selected) = nnz(selected);
end

isCanonical = false(snapshotCount, 1);
completeIndices = find(completeMask);
[~, firstIndices] = unique(hashes(completeMask), 'stable');
isCanonical(completeIndices(firstIndices)) = true;

metadataNames = {'source_file', 'scenario', 'segment', 'snapshot_id', ...
    'begin_frame', 'content_sha256', 'status'};
features = snapshots(:, metadataNames);
features.complete = completeMask;
features.duplicate_count = duplicateCount;
features.is_canonical = isCanonical;
features.is_excluded = isExcluded;
features.exclusion_reason = exclusionReason;
features = [features, array2table(featureValues, ...
    VariableNames=cellstr(allFeatureNames))];

featuresPath = fullfile(dataDirectory, 'navigation_features.csv');
writetable(features, featuresPath);

labelsPath = fullfile(dataDirectory, 'navigation_labels.csv');
labelColumns = {'content_sha256', 'snapshot_id', 'source_file', 'scenario', ...
    'segment', 'begin_frame'};
labels = features(features.complete & features.is_canonical, labelColumns);
labels.ground_truth = repmat("UNLABELED", height(labels), 1);
labels.notes = repmat("", height(labels), 1);
labels.motion_context = repmat("UNLABELED", height(labels), 1);
labels.motion_notes = repmat("", height(labels), 1);
labels.ground_truth(labels.scenario == "safe") = "SAFE";

if isfile(labelsPath)
    previous = readtable(labelsPath, TextType='string', ...
        VariableNamingRule='preserve');
    previousByHash = containers.Map('KeyType', 'char', 'ValueType', 'double');
    for index = 1:height(previous)
        previousByHash(char(previous.content_sha256(index))) = index;
    end
    for index = 1:height(labels)
        key = char(labels.content_sha256(index));
        if isKey(previousByHash, key)
            oldIndex = previousByHash(key);
            labels.ground_truth(index) = previous.ground_truth(oldIndex);
            labels.notes(index) = previous.notes(oldIndex);
            if ismember('motion_context', previous.Properties.VariableNames)
                labels.motion_context(index) = previous.motion_context(oldIndex);
            end
            if ismember('motion_notes', previous.Properties.VariableNames)
                labels.motion_notes(index) = previous.motion_notes(oldIndex);
            end
        end
    end
end

labelFeatureLocation = zeros(height(labels), 1);
for index = 1:height(labels)
    labelFeatureLocation(index) = find(features.content_sha256 == ...
        labels.content_sha256(index), 1);
end
excludedLabels = features.is_excluded(labelFeatureLocation);
labels.ground_truth(excludedLabels) = "EXCLUDED";
labels.notes(excludedLabels) = features.exclusion_reason(labelFeatureLocation(excludedLabels));
writetable(labels, labelsPath);

fprintf(['Feature rows: %d total, %d complete, %d independent complete, ' ...
    '%d excluded independent.\n'], ...
    height(features), nnz(features.complete), nnz(features.is_canonical), ...
    nnz(features.is_canonical & features.is_excluded));
fprintf('Labels: %d SAFE, %d DANGER, %d EXCLUDED, %d UNLABELED.\n', ...
    nnz(labels.ground_truth == "SAFE"), ...
    nnz(labels.ground_truth == "DANGER"), ...
    nnz(labels.ground_truth == "EXCLUDED"), ...
    nnz(labels.ground_truth == "UNLABELED"));
end

function [isExcluded, reason] = local_exclusion_mask(snapshots, dataDirectory)
isExcluded = false(height(snapshots), 1);
reason = repmat("", height(snapshots), 1);
exclusionPath = fullfile(dataDirectory, 'navigation_exclusions.csv');
if ~isfile(exclusionPath)
    return;
end
rules = readtable(exclusionPath, TextType='string', ...
    VariableNamingRule='preserve');
for ruleIndex = 1:height(rules)
    selected = snapshots.source_file == rules.source_file(ruleIndex) & ...
        snapshots.segment == rules.segment(ruleIndex);
    isExcluded(selected) = true;
    reason(selected) = rules.reason(ruleIndex);
end
end

function [names, gridNames, temporalNames] = local_feature_names()
names = [ ...
    "dark_all", "dark_left", "dark_center", "dark_right", ...
    "dark_top_half", "dark_bottom_half", ...
    "dark_left_edge", "dark_right_edge", ...
    "row_dark_min", "row_dark_max", "row_dark_std", ...
    "column_dark_min", "column_dark_max", "column_dark_std", ...
    "center_row_dark_min", "center_row_dark_max", "center_row_dark_std", ...
    "center_column_dark_min", "center_column_dark_max", "center_column_dark_std", ...
    "white_left_border_mean", "white_left_border_max", ...
    "white_right_border_mean", "white_right_border_max", ...
    "white_top_border_mean", "white_top_border_max", ...
    "white_bottom_border_mean", "white_bottom_border_max", ...
    "internal_white_mean", "internal_white_max", ...
    "row_transition_mean", "row_transition_max", ...
    "longest_white_run_mean", "longest_white_run_max", ...
    "vertical_white_columns_75", "horizontal_white_rows_75", ...
    "dark_bbox_width", "dark_bbox_height", ...
    "white_centroid_x", "white_centroid_y", ...
    "white_cov_xx", "white_cov_yy", "white_cov_xy", ...
    "bottom4_dark_left", "bottom4_dark_center", "bottom4_dark_right", ...
    "bottom_white_run_mean", "bottom_white_run_max", ...
    "bottom_white_run_std", ...
    "bottom_edge_valid_percent", "bottom_edge_slope", ...
    "bottom_edge_residual", ...
    "left_edge_valid_percent", "left_edge_slope", ...
    "left_edge_residual", ...
    "right_edge_valid_percent", "right_edge_slope", ...
    "right_edge_residual"];
gridNames = strings(1, 20);
nameIndex = 1;
for row = 1:4
    for column = 1:5
        gridNames(nameIndex) = sprintf('grid_dark_r%d_c%d', row, column);
        nameIndex = nameIndex + 1;
    end
end
temporalNames = [ ...
    "rate_dark_all", "rate_dark_left", "rate_dark_center", ...
    "rate_dark_right", "rate_dark_bottom_half", ...
    "rate_bottom_white_run_mean", "rate_bottom_edge_slope", ...
    "rate_white_centroid_x"];
end

function values = local_image_features(dark)
center = dark(:, 68:134);
rowDark = mean(dark, 2) * 100;
columnDark = mean(dark, 1) * 100;
centerRowDark = mean(center, 2) * 100;
centerColumnDark = mean(center, 1) * 100;

[leftWhite, rightWhite, internalWhite, longestWhite] = ...
    local_horizontal_white_features(dark);
[topWhite, bottomWhite] = local_vertical_white_features(dark);
rowTransitions = sum(diff(dark, 1, 2) ~= 0, 2);
[centroidX, centroidY, covXX, covYY, covXY] = ...
    local_white_geometry(dark);
[bottomValid, bottomSlope, bottomResidual] = ...
    local_edge_fit(bottomWhite(:));
[leftValid, leftSlope, leftResidual] = local_edge_fit(leftWhite(:));
[rightValid, rightSlope, rightResidual] = local_edge_fit(rightWhite(:));

[darkRows, darkColumns] = find(dark);
if isempty(darkRows)
    bboxWidth = 0;
    bboxHeight = 0;
else
    bboxWidth = (max(darkColumns) - min(darkColumns) + 1) * 100 / size(dark, 2);
    bboxHeight = (max(darkRows) - min(darkRows) + 1) * 100 / size(dark, 1);
end

values = [ ...
    mean(dark, 'all') * 100, ...
    mean(dark(:, 1:67), 'all') * 100, ...
    mean(center, 'all') * 100, ...
    mean(dark(:, 135:200), 'all') * 100, ...
    mean(dark(1:12, :), 'all') * 100, ...
    mean(dark(13:24, :), 'all') * 100, ...
    mean(dark(:, 1:20), 'all') * 100, ...
    mean(dark(:, 181:200), 'all') * 100, ...
    min(rowDark), max(rowDark), std(rowDark), ...
    min(columnDark), max(columnDark), std(columnDark), ...
    min(centerRowDark), max(centerRowDark), std(centerRowDark), ...
    min(centerColumnDark), max(centerColumnDark), std(centerColumnDark), ...
    mean(leftWhite), max(leftWhite), mean(rightWhite), max(rightWhite), ...
    mean(topWhite), max(topWhite), mean(bottomWhite), max(bottomWhite), ...
    mean(internalWhite), max(internalWhite), ...
    mean(rowTransitions), max(rowTransitions), ...
    mean(longestWhite), max(longestWhite), ...
    nnz(mean(~dark, 1) >= 0.75) * 100 / size(dark, 2), ...
    nnz(mean(~dark, 2) >= 0.75) * 100 / size(dark, 1), ...
    bboxWidth, bboxHeight, ...
    centroidX, centroidY, covXX, covYY, covXY, ...
    mean(dark(21:24, 1:67), 'all') * 100, ...
    mean(dark(21:24, 68:134), 'all') * 100, ...
    mean(dark(21:24, 135:200), 'all') * 100, ...
    mean(bottomWhite), max(bottomWhite), std(bottomWhite), ...
    bottomValid, bottomSlope, bottomResidual, ...
    leftValid, leftSlope, leftResidual, ...
    rightValid, rightSlope, rightResidual];

rowEdges = round(linspace(1, 25, 5));
columnEdges = round(linspace(1, 201, 6));
for row = 1:4
    for column = 1:5
        block = dark(rowEdges(row):(rowEdges(row + 1) - 1), ...
                     columnEdges(column):(columnEdges(column + 1) - 1));
        values(end + 1) = mean(block, 'all') * 100; %#ok<AGROW>
    end
end
end

function featureValues = local_temporal_features(snapshots, completeMask, ...
        featureValues, spatialNames, temporalNames)
temporalStart = numel(spatialNames) + 1;
sourceFeatures = ["dark_all", "dark_left", "dark_center", "dark_right", ...
    "dark_bottom_half", "bottom_white_run_mean", "bottom_edge_slope", ...
    "white_centroid_x"];
sourceIndices = zeros(1, numel(sourceFeatures));
for index = 1:numel(sourceFeatures)
    sourceIndices(index) = find(spatialNames == sourceFeatures(index), 1);
end
if numel(temporalNames) ~= numel(sourceIndices)
    error('Temporal feature name and source feature counts do not match.');
end

groupKeys = snapshots.source_file + "|" + string(snapshots.segment);
for group = unique(groupKeys, 'stable').'
    selected = find(completeMask & groupKeys == group);
    [~, order] = sort(snapshots.begin_source_line(selected));
    selected = selected(order);
    previousIndex = 0;
    for currentIndex = selected.'
        if 0 == previousIndex
            rates = zeros(1, numel(sourceIndices));
        else
            frameDelta = double(snapshots.begin_frame(currentIndex)) - ...
                double(snapshots.begin_frame(previousIndex));
            if ~isfinite(frameDelta) || frameDelta <= 0
                frameDelta = 1;
            end
            rates = (featureValues(currentIndex, sourceIndices) - ...
                featureValues(previousIndex, sourceIndices)) / frameDelta;
        end
        featureValues(currentIndex, ...
            temporalStart:(temporalStart + numel(temporalNames) - 1)) = rates;
        previousIndex = currentIndex;
    end
end
end

function [centroidX, centroidY, covXX, covYY, covXY] = ...
        local_white_geometry(dark)
[whiteY, whiteX] = find(~dark);
if isempty(whiteX)
    centroidX = 0;
    centroidY = 0;
    covXX = 0;
    covYY = 0;
    covXY = 0;
    return;
end
x = (double(whiteX) - 1) * 100 / max(1, size(dark, 2) - 1);
y = (double(whiteY) - 1) * 100 / max(1, size(dark, 1) - 1);
centroidX = mean(x);
centroidY = mean(y);
centeredX = x - centroidX;
centeredY = y - centroidY;
covXX = mean(centeredX .^ 2);
covYY = mean(centeredY .^ 2);
covXY = mean(centeredX .* centeredY);
end

function [validPercent, slope, residual] = local_edge_fit(intrusion)
sampleCount = numel(intrusion);
axisPercent = ((0:(sampleCount - 1)).' * 100) / max(1, sampleCount - 1);
valid = intrusion > 0 & intrusion < 100;
validPercent = nnz(valid) * 100 / sampleCount;
if nnz(valid) < 2
    slope = 0;
    residual = 0;
    return;
end
design = [axisPercent(valid), ones(nnz(valid), 1)];
coefficients = design \ intrusion(valid);
fitted = design * coefficients;
slope = coefficients(1);
residual = sqrt(mean((intrusion(valid) - fitted) .^ 2));
end

function [leftWhite, rightWhite, internalWhite, longestWhite] = ...
    local_horizontal_white_features(dark)
height = size(dark, 1);
width = size(dark, 2);
leftWhite = zeros(height, 1);
rightWhite = zeros(height, 1);
internalWhite = zeros(height, 1);
longestWhite = zeros(height, 1);
for row = 1:height
    firstDark = find(dark(row, :), 1, 'first');
    lastDark = find(dark(row, :), 1, 'last');
    if isempty(firstDark)
        leftWhite(row) = 100;
        rightWhite(row) = 100;
        internalWhite(row) = 100;
    else
        leftWhite(row) = (firstDark - 1) * 100 / width;
        rightWhite(row) = (width - lastDark) * 100 / width;
        internalWhite(row) = nnz(~dark(row, firstDark:lastDark)) * 100 / width;
    end
    longestWhite(row) = local_longest_run(~dark(row, :)) * 100 / width;
end
end

function [topWhite, bottomWhite] = local_vertical_white_features(dark)
width = size(dark, 2);
height = size(dark, 1);
topWhite = zeros(1, width);
bottomWhite = zeros(1, width);
for column = 1:width
    firstDark = find(dark(:, column), 1, 'first');
    lastDark = find(dark(:, column), 1, 'last');
    if isempty(firstDark)
        topWhite(column) = 100;
        bottomWhite(column) = 100;
    else
        topWhite(column) = (firstDark - 1) * 100 / height;
        bottomWhite(column) = (height - lastDark) * 100 / height;
    end
end
end

function longest = local_longest_run(values)
longest = 0;
current = 0;
for index = 1:numel(values)
    if values(index)
        current = current + 1;
        longest = max(longest, current);
    else
        current = 0;
    end
end
end

function values = local_logical_column(column)
if islogical(column)
    values = column;
elseif isnumeric(column)
    values = column ~= 0;
else
    values = strcmpi(string(column), "true");
end
end
