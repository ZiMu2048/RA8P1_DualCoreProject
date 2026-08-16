function label_navigation_samples(dataDirectory, reviewMode)
%LABEL_NAVIGATION_SAMPLES Assign physical SAFE/DANGER labels to unique images.
% Modes: unlabeled (default), labeled, all, or misclassified.
% EXCLUDED rows are always blocked. Misclassified rows come from model output.
% Keys: S=safe, D=danger, C=seam, U=uncertain, R=reset,
%       B=back, K=keep, Q=quit.

if nargin < 1 || strlength(string(dataDirectory)) == 0
    scriptDirectory = fileparts(mfilename('fullpath'));
    dataDirectory = fullfile(scriptDirectory, '..', 'data', 'navigation_20260816');
end
if nargin < 2 || strlength(string(reviewMode)) == 0
    reviewMode = "unlabeled";
end
reviewMode = lower(string(reviewMode));
if ~ismember(reviewMode, ["unlabeled", "labeled", "all", "misclassified"])
    error(['reviewMode must be "unlabeled", "labeled", "all", ' ...
        'or "misclassified".']);
end

labelsPath = fullfile(dataDirectory, 'navigation_labels.csv');
features = readtable(fullfile(dataDirectory, 'navigation_features.csv'), ...
    TextType='string', VariableNamingRule='preserve');
labels = readtable(labelsPath, TextType='string', VariableNamingRule='preserve');

binaryRowsPath = fullfile(dataDirectory, 'binary_rows.csv');
options = detectImportOptions(binaryRowsPath, ...
    TextType='string', VariableNamingRule='preserve');
options = setvartype(options, 'bits', 'string');
rows = readtable(binaryRowsPath, options);

figureHandle = figure(Color='white', Name='防跌落安全标注', ...
    NumberTitle='off', Position=[120 120 1250 620], ...
    KeyPressFcn=@local_key_press, CloseRequestFcn=@local_close_request);
axesHandle = axes(Parent=figureHandle, Position=[0.05 0.18 0.90 0.75]);
local_add_button(figureHandle, [0.01 0.04 0.09 0.07], '安全 (S)', ...
    [0.08 0.32 0.16], "SAFE");
local_add_button(figureHandle, [0.105 0.04 0.10 0.07], '危险 (D)', ...
    [0.48 0.08 0.08], "DANGER");
local_add_button(figureHandle, [0.21 0.04 0.09 0.07], '接缝 (C)', ...
    [0.08 0.22 0.42], "SEAM");
local_add_button(figureHandle, [0.305 0.04 0.12 0.07], '不确定 (U)', ...
    [0.38 0.32 0.05], "UNCERTAIN");
local_add_button(figureHandle, [0.43 0.04 0.10 0.07], '撤销 (R)', ...
    [0.36 0.20 0.08], "RESET");
local_add_button(figureHandle, [0.535 0.04 0.085 0.07], '上一张 (B)', ...
    [0.12 0.24 0.30], "BACK");
local_add_button(figureHandle, [0.625 0.04 0.075 0.07], '保持 (K)', ...
    [0.20 0.20 0.20], "SKIP");
uicontrol(figureHandle, Style='pushbutton', Units='normalized', ...
    Position=[0.705 0.04 0.11 0.07], String='导出图片', ...
    BackgroundColor=[0.16 0.24 0.28], ForegroundColor='white', ...
    FontWeight='bold', ...
    Callback=@(~, ~) local_export_png(figureHandle, axesHandle));
local_add_button(figureHandle, [0.82 0.04 0.17 0.07], ...
    '保存并退出 (Q)', [0.14 0.14 0.14], "QUIT");

reviewDescriptions = strings(0, 1);
switch reviewMode
    case "unlabeled"
        reviewMask = labels.ground_truth == "UNLABELED";
        reviewIndices = find(reviewMask);
    case "labeled"
        reviewMask = labels.ground_truth ~= "UNLABELED" & ...
            labels.ground_truth ~= "EXCLUDED";
        reviewIndices = find(reviewMask);
    case "all"
        reviewMask = labels.ground_truth ~= "EXCLUDED";
        reviewIndices = find(reviewMask);
    case "misclassified"
        errorsPath = fullfile(dataDirectory, 'model', ...
            'navigation_tree_misclassified.csv');
        if ~isfile(errorsPath)
            error(['Misclassified sample list not found: %s\nRun ' ...
                'train_navigation_tree first.'], errorsPath);
        end
        errors = readtable(errorsPath, TextType='string', ...
            VariableNamingRule='preserve');
        requiredColumns = ["content_sha256", "ground_truth", ...
            "group_validation_prediction"];
        if ~all(ismember(requiredColumns, ...
                string(errors.Properties.VariableNames)))
            error('Misclassified sample list does not have the required columns.');
        end
        errors = errors(errors.ground_truth ~= ...
            errors.group_validation_prediction, :);
        missedDanger = errors.ground_truth == "DANGER" & ...
            errors.group_validation_prediction == "SAFE";
        errors = [errors(missedDanger, :); errors(~missedDanger, :)];
        [found, labelLocations] = ismember(errors.content_sha256, ...
            labels.content_sha256);
        errors = errors(found, :);
        reviewIndices = labelLocations(found);
        allowed = labels.ground_truth(reviewIndices) ~= "EXCLUDED";
        reviewIndices = reviewIndices(allowed);
        errors = errors(allowed, :);
        reviewDescriptions = repmat("FALSE_DANGER: validation predicted DANGER", ...
            height(errors), 1);
        missedDanger = errors.ground_truth == "DANGER" & ...
            errors.group_validation_prediction == "SAFE";
        reviewDescriptions(missedDanger) = ...
            "MISSED_DANGER: validation predicted SAFE";
end

reviewPosition = 1;
while reviewPosition <= numel(reviewIndices)
    labelIndex = reviewIndices(reviewPosition);
    if ~isvalid(figureHandle)
        break;
    end

    featureIndex = find(features.content_sha256 == ...
        labels.content_sha256(labelIndex), 1);
    if isempty(featureIndex)
        error('Feature row not found for hash %s.', labels.content_sha256(labelIndex));
    end
    imageData = local_snapshot_image(rows, labels.snapshot_id(labelIndex));

    imagesc(axesHandle, imageData);
    axis(axesHandle, 'image', 'tight');
    colormap(axesHandle, gray(256));
    xlabel(axesHandle, 'x');
    ylabel(axesHandle, 'ROI row');
    titleText = sprintf(['%d/%d  %s  segment=%g frame=%g  ' ...
        'd128=%.1f/%.1f/%.1f\n当前标签=%s  模式=%s'], ...
        reviewPosition, numel(reviewIndices), labels.scenario(labelIndex), ...
        labels.segment(labelIndex), labels.begin_frame(labelIndex), ...
        features.dark_left(featureIndex), features.dark_center(featureIndex), ...
        features.dark_right(featureIndex), labels.ground_truth(labelIndex), ...
        reviewMode);
    if ~isempty(reviewDescriptions)
        titleText = sprintf('%s\n%s', titleText, ...
            reviewDescriptions(reviewPosition));
    end
    title(axesHandle, titleText, Interpreter='none');
    exportName = sprintf('%s_segment_%g_frame_%g.png', ...
        regexprep(char(labels.scenario(labelIndex)), '[<>:"/\\|?*]', '_'), ...
        labels.segment(labelIndex), labels.begin_frame(labelIndex));
    setappdata(figureHandle, 'export_path', ...
        fullfile(dataDirectory, 'label_exports', exportName));
    setappdata(figureHandle, 'label_action', "");
    drawnow;

    uiwait(figureHandle);
    if ~isvalid(figureHandle)
        local_save(labels, labelsPath);
        return;
    end
    action = string(getappdata(figureHandle, 'label_action'));
    switch action
        case "SAFE"
            labels.ground_truth(labelIndex) = "SAFE";
        case "DANGER"
            labels.ground_truth(labelIndex) = "DANGER";
        case "SEAM"
            labels.ground_truth(labelIndex) = "SEAM";
        case "UNCERTAIN"
            labels.ground_truth(labelIndex) = "UNCERTAIN";
        case "RESET"
            labels.ground_truth(labelIndex) = "UNLABELED";
        case "BACK"
            local_save(labels, labelsPath);
            reviewPosition = max(1, reviewPosition - 1);
            continue;
        case "SKIP"
            % Keep the current label and advance for this session.
        case "QUIT"
            local_save(labels, labelsPath);
            local_delete_figure(figureHandle);
            fprintf('Labeling stopped at row %d.\n', labelIndex);
            return;
    end
    local_save(labels, labelsPath);
    reviewPosition = reviewPosition + 1;
end

if isvalid(figureHandle)
    local_delete_figure(figureHandle);
end
local_save(labels, labelsPath);
fprintf(['Labeling complete. SAFE=%d DANGER=%d SEAM=%d UNCERTAIN=%d ' ...
    'UNLABELED=%d.\n'], ...
    nnz(labels.ground_truth == "SAFE"), ...
    nnz(labels.ground_truth == "DANGER"), ...
    nnz(labels.ground_truth == "SEAM"), ...
    nnz(labels.ground_truth == "UNCERTAIN"), ...
    nnz(labels.ground_truth == "UNLABELED"));
end

function local_add_button(figureHandle, position, label, color, action)
uicontrol(figureHandle, Style='pushbutton', Units='normalized', ...
    Position=position, String=label, BackgroundColor=color, ...
    ForegroundColor='white', FontWeight='bold', ...
    Callback=@(~, ~) local_choose(figureHandle, action));
end

function local_choose(figureHandle, action)
if ~isvalid(figureHandle)
    return;
end
setappdata(figureHandle, 'label_action', action);
uiresume(figureHandle);
end

function local_key_press(figureHandle, event)
switch lower(string(event.Key))
    case "s"
        local_choose(figureHandle, "SAFE");
    case "d"
        local_choose(figureHandle, "DANGER");
    case "c"
        local_choose(figureHandle, "SEAM");
    case "u"
        local_choose(figureHandle, "UNCERTAIN");
    case "r"
        local_choose(figureHandle, "RESET");
    case "b"
        local_choose(figureHandle, "BACK");
    case "k"
        local_choose(figureHandle, "SKIP");
    case "q"
        local_choose(figureHandle, "QUIT");
end
end

function local_close_request(figureHandle, ~)
local_choose(figureHandle, "QUIT");
end

function local_export_png(figureHandle, axesHandle)
if ~isvalid(figureHandle) || ~isvalid(axesHandle)
    return;
end
exportPath = string(getappdata(figureHandle, 'export_path'));
exportDirectory = fileparts(exportPath);
if ~isfolder(exportDirectory)
    mkdir(exportDirectory);
end
exportgraphics(axesHandle, exportPath, Resolution=220);
fprintf('Exported %s\n', exportPath);
end

function local_delete_figure(figureHandle)
if isvalid(figureHandle)
    figureHandle.CloseRequestFcn = [];
    delete(figureHandle);
end
end

function local_save(labels, labelsPath)
writetable(labels, labelsPath);
end

function imageData = local_snapshot_image(rows, snapshotId)
selected = rows(rows.snapshot_id == snapshotId, :);
[~, order] = sort(selected.row_index);
selected = selected(order, :);
if height(selected) ~= 24
    error('Snapshot %s has %d rows instead of 24.', snapshotId, height(selected));
end
bits = char(selected.bits);
if size(bits, 2) ~= 200
    error('Snapshot %s has width %d instead of 200.', snapshotId, size(bits, 2));
end
imageData = 1.0 - double(bits == '1');
end
