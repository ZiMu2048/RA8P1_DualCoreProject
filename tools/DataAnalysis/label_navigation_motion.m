function label_navigation_motion(dataDirectory, reviewMode)
%LABEL_NAVIGATION_MOTION Label actual vehicle motion for existing snapshots.
% Modes: unlabeled (default), labeled, or all. EXCLUDED rows are blocked.
% Keys: W=forward, S=reverse, A=left, D=right, X=stationary,
%       U=unknown, R=reset, B=back, K=keep, Q=quit.

if nargin < 1 || strlength(string(dataDirectory)) == 0
    scriptDirectory = fileparts(mfilename('fullpath'));
    dataDirectory = fullfile(scriptDirectory, '..', 'data', ...
        'navigation_20260816');
end
if nargin < 2 || strlength(string(reviewMode)) == 0
    reviewMode = "unlabeled";
end
reviewMode = lower(string(reviewMode));
if ~ismember(reviewMode, ["unlabeled", "labeled", "all"])
    error('reviewMode must be "unlabeled", "labeled", or "all".');
end

labelsPath = fullfile(dataDirectory, 'navigation_labels.csv');
features = readtable(fullfile(dataDirectory, 'navigation_features.csv'), ...
    TextType='string', VariableNamingRule='preserve');
labels = readtable(labelsPath, TextType='string', ...
    VariableNamingRule='preserve');
if ~ismember('motion_context', labels.Properties.VariableNames)
    labels.motion_context = repmat("UNLABELED", height(labels), 1);
end
if ~ismember('motion_notes', labels.Properties.VariableNames)
    labels.motion_notes = repmat("", height(labels), 1);
end
local_save(labels, labelsPath);

binaryRowsPath = fullfile(dataDirectory, 'binary_rows.csv');
options = detectImportOptions(binaryRowsPath, ...
    TextType='string', VariableNamingRule='preserve');
options = setvartype(options, 'bits', 'string');
rows = readtable(binaryRowsPath, options);

switch reviewMode
    case "unlabeled"
        reviewMask = labels.motion_context == "UNLABELED" & ...
            labels.ground_truth ~= "EXCLUDED";
    case "labeled"
        reviewMask = labels.motion_context ~= "UNLABELED" & ...
            labels.ground_truth ~= "EXCLUDED";
    case "all"
        reviewMask = labels.ground_truth ~= "EXCLUDED";
end
reviewIndices = find(reviewMask);

figureHandle = figure(Color='white', Name='车辆运动上下文标注', ...
    NumberTitle='off', Position=[120 80 1250 700], ...
    KeyPressFcn=@local_key_press, CloseRequestFcn=@local_close_request);
axesHandle = axes(Parent=figureHandle, Position=[0.05 0.27 0.90 0.68]);

local_add_button(figureHandle, [0.01 0.145 0.15 0.065], ...
    '前进 (W)', [0.06 0.30 0.16], "FORWARD");
local_add_button(figureHandle, [0.17 0.145 0.15 0.065], ...
    '后退 (S)', [0.30 0.16 0.06], "REVERSE");
local_add_button(figureHandle, [0.33 0.145 0.15 0.065], ...
    '左转 (A)', [0.08 0.20 0.40], "TURN_LEFT");
local_add_button(figureHandle, [0.49 0.145 0.15 0.065], ...
    '右转 (D)', [0.30 0.10 0.36], "TURN_RIGHT");
local_add_button(figureHandle, [0.65 0.145 0.16 0.065], ...
    '静止 (X)', [0.22 0.22 0.22], "STATIONARY");
local_add_button(figureHandle, [0.82 0.145 0.17 0.065], ...
    '不确定 (U)', [0.38 0.32 0.05], "UNKNOWN");

local_add_button(figureHandle, [0.01 0.045 0.13 0.065], ...
    '撤销 (R)', [0.36 0.20 0.08], "RESET");
local_add_button(figureHandle, [0.15 0.045 0.13 0.065], ...
    '上一张 (B)', [0.12 0.24 0.30], "BACK");
local_add_button(figureHandle, [0.29 0.045 0.13 0.065], ...
    '保持 (K)', [0.20 0.20 0.20], "SKIP");
uicontrol(figureHandle, Style='pushbutton', Units='normalized', ...
    Position=[0.43 0.045 0.17 0.065], String='导出图片', ...
    BackgroundColor=[0.16 0.24 0.28], ForegroundColor='white', ...
    FontWeight='bold', ...
    Callback=@(~, ~) local_export_png(figureHandle, axesHandle));
local_add_button(figureHandle, [0.70 0.045 0.29 0.065], ...
    '保存并退出 (Q)', [0.14 0.14 0.14], "QUIT");

reviewPosition = 1;
while reviewPosition <= numel(reviewIndices)
    labelIndex = reviewIndices(reviewPosition);
    if ~isvalid(figureHandle)
        break;
    end

    featureIndex = find(features.content_sha256 == ...
        labels.content_sha256(labelIndex), 1);
    if isempty(featureIndex)
        error('Feature row not found for hash %s.', ...
            labels.content_sha256(labelIndex));
    end
    imageData = local_snapshot_image(rows, labels.snapshot_id(labelIndex));

    imagesc(axesHandle, imageData);
    axis(axesHandle, 'image', 'tight');
    colormap(axesHandle, gray(256));
    xlabel(axesHandle, 'x');
    ylabel(axesHandle, 'ROI row');
    title(axesHandle, sprintf(['%d/%d  %s  segment=%g frame=%g  ' ...
        'd128=%.1f/%.1f/%.1f\n安全标签=%s  运动=%s  模式=%s'], ...
        reviewPosition, numel(reviewIndices), labels.scenario(labelIndex), ...
        labels.segment(labelIndex), labels.begin_frame(labelIndex), ...
        features.dark_left(featureIndex), features.dark_center(featureIndex), ...
        features.dark_right(featureIndex), labels.ground_truth(labelIndex), ...
        labels.motion_context(labelIndex), reviewMode), Interpreter='none');
    exportName = sprintf('motion_%s_segment_%g_frame_%g.png', ...
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
        case {"FORWARD", "REVERSE", "TURN_LEFT", "TURN_RIGHT", ...
                "STATIONARY", "UNKNOWN"}
            labels.motion_context(labelIndex) = action;
        case "RESET"
            labels.motion_context(labelIndex) = "UNLABELED";
        case "BACK"
            local_save(labels, labelsPath);
            reviewPosition = max(1, reviewPosition - 1);
            continue;
        case "SKIP"
            % Keep the current motion label and advance for this session.
        case "QUIT"
            local_save(labels, labelsPath);
            local_delete_figure(figureHandle);
            fprintf('Motion labeling stopped at row %d.\n', labelIndex);
            return;
    end
    local_save(labels, labelsPath);
    reviewPosition = reviewPosition + 1;
end

if isvalid(figureHandle)
    local_delete_figure(figureHandle);
end
local_save(labels, labelsPath);
names = ["FORWARD", "REVERSE", "TURN_LEFT", "TURN_RIGHT", ...
    "STATIONARY", "UNKNOWN", "UNLABELED"];
eligible = labels.ground_truth ~= "EXCLUDED";
fprintf(['Motion labeling complete. FORWARD=%d REVERSE=%d LEFT=%d ' ...
    'RIGHT=%d STATIONARY=%d UNKNOWN=%d UNLABELED=%d EXCLUDED=%d.\n'], ...
    nnz(eligible & labels.motion_context == names(1)), ...
    nnz(eligible & labels.motion_context == names(2)), ...
    nnz(eligible & labels.motion_context == names(3)), ...
    nnz(eligible & labels.motion_context == names(4)), ...
    nnz(eligible & labels.motion_context == names(5)), ...
    nnz(eligible & labels.motion_context == names(6)), ...
    nnz(eligible & labels.motion_context == names(7)), ...
    nnz(~eligible));
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
    case "w"
        local_choose(figureHandle, "FORWARD");
    case "s"
        local_choose(figureHandle, "REVERSE");
    case "a"
        local_choose(figureHandle, "TURN_LEFT");
    case "d"
        local_choose(figureHandle, "TURN_RIGHT");
    case "x"
        local_choose(figureHandle, "STATIONARY");
    case "u"
        local_choose(figureHandle, "UNKNOWN");
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
