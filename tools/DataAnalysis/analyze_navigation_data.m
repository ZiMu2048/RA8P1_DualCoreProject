function analyze_navigation_data(dataDirectory, outputDirectory)
%ANALYZE_NAVIGATION_DATA Plot normalized RTT data without filtering samples.

if nargin < 1 || strlength(string(dataDirectory)) == 0
    scriptDirectory = fileparts(mfilename('fullpath'));
    dataDirectory = fullfile(scriptDirectory, '..', 'data', 'navigation_20260816');
end
if nargin < 2 || strlength(string(outputDirectory)) == 0
    outputDirectory = fullfile(dataDirectory, 'plots');
end
if ~isfolder(outputDirectory)
    mkdir(outputDirectory);
end

nav = readtable(fullfile(dataDirectory, 'nav_metrics.csv'), ...
    TextType='string', VariableNamingRule='preserve');
snapshots = readtable(fullfile(dataDirectory, 'binary_snapshots.csv'), ...
    TextType='string', VariableNamingRule='preserve');
binaryRowsPath = fullfile(dataDirectory, 'binary_rows.csv');
binaryRowsOptions = detectImportOptions(binaryRowsPath, ...
    TextType='string', VariableNamingRule='preserve');
binaryRowsOptions = setvartype(binaryRowsOptions, 'bits', 'string');
rows = readtable(binaryRowsPath, binaryRowsOptions);

scenarioNames = unique(nav.scenario, 'stable');
figure('Visible', 'off', 'Color', 'white', 'Position', [80 80 1500 1700]);
layout = tiledlayout(numel(scenarioNames), 1, TileSpacing='compact', Padding='compact');
title(layout, 'Raw navigation metrics in source order');
for index = 1:numel(scenarioNames)
    nexttile;
    selected = nav.scenario == scenarioNames(index);
    x = nav.source_line(selected);
    plot(x, nav.d128_left(selected), Color=[0.35 0.65 0.85], LineWidth=0.8);
    hold on;
    plot(x, nav.d128_center(selected), Color=[0.05 0.05 0.05], LineWidth=1.5);
    plot(x, nav.d128_right(selected), Color=[0.85 0.45 0.25], LineWidth=0.8);
    yline(65, '--r', '65');
    yline(80, '--g', '80');
    ylim([0 100]);
    grid on;
    ylabel('d128 (%)');
    title(scenarioNames(index), Interpreter='none');
    if index == numel(scenarioNames)
        xlabel('Source line (preserves capture order)');
    end
end
legend({'Left', 'Center', 'Right', 'Danger threshold', 'Safe threshold'}, ...
    Location='southoutside', Orientation='horizontal');
exportgraphics(gcf, fullfile(outputDirectory, 'raw_d128_by_scenario.png'), Resolution=180);
close(gcf);

figure('Visible', 'off', 'Color', 'white', 'Position', [80 80 1500 900]);
layout = tiledlayout(2, 3, TileSpacing='compact', Padding='compact');
title(layout, 'Center d128 distributions from NAV log records');
for index = 1:numel(scenarioNames)
    nexttile;
    selected = nav.scenario == scenarioNames(index);
    histogram(nav.d128_center(selected), 0:5:100, FaceColor=[0.25 0.45 0.65]);
    hold on;
    xline(65, '--r');
    xline(80, '--g');
    xlim([0 100]);
    grid on;
    xlabel('Center d128 (%)');
    ylabel('Record count');
    title(scenarioNames(index), Interpreter='none');
end
exportgraphics(gcf, fullfile(outputDirectory, 'center_d128_histograms.png'), Resolution=180);
close(gcf);

completeMask = local_logical_column(snapshots.complete);
safeSnapshots = snapshots(completeMask & snapshots.scenario == "safe", :);
if height(safeSnapshots) >= 3
    [~, order] = sort(safeSnapshots.d128_center_percent_exact);
    chosen = safeSnapshots(order([1, round(numel(order) / 2), end]), :);
    figure('Visible', 'off', 'Color', 'white', 'Position', [80 80 1500 420]);
    layout = tiledlayout(1, 3, TileSpacing='compact', Padding='compact');
    title(layout, 'Unfiltered complete binary snapshots from safe.txt');
    for index = 1:3
        nexttile;
        imageData = local_snapshot_image(rows, chosen.snapshot_id(index));
        imagesc(imageData);
        axis image tight;
        colormap(gray(256));
        title(sprintf('frame %g, center d128 %.2f%%', ...
            chosen.begin_frame(index), chosen.d128_center_percent_exact(index)));
        xlabel('x');
        ylabel('ROI row');
    end
    exportgraphics(gcf, fullfile(outputDirectory, 'safe_binary_examples.png'), Resolution=220);
    close(gcf);
end

fprintf('Plots written to %s\n', outputDirectory);
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

function imageData = local_snapshot_image(rows, snapshotId)
selected = rows(rows.snapshot_id == snapshotId, :);
[~, order] = sort(selected.row_index);
selected = selected(order, :);
if height(selected) ~= 24
    error('Snapshot %s has %d rows instead of 24.', snapshotId, height(selected));
end
bits = char(selected.bits);
if size(bits, 2) ~= 200
    error('Snapshot %s has a row width other than 200.', snapshotId);
end
imageData = 1.0 - double(bits == '1');
end
