param(
    [Parameter(Mandatory = $true)]
    [string[]] $InputFiles,

    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$rawRecords = [System.Collections.Generic.List[object]]::new()
$navMetrics = [System.Collections.Generic.List[object]]::new()
$binaryRows = [System.Collections.Generic.List[object]]::new()
$snapshots = [System.Collections.Generic.List[object]]::new()
$snapshotDuplicates = [System.Collections.Generic.List[object]]::new()
$manifest = [System.Collections.Generic.List[object]]::new()

function Add-SnapshotSummary
{
    param(
        [Parameter(Mandatory = $true)] $Snapshot,
        [Parameter(Mandatory = $true)] [string] $CloseReason,
        [Parameter(Mandatory = $true)] $Destination
    )

    $rowCount = $Snapshot.Rows.Count
    $darkLeft = 0
    $darkCenter = 0
    $darkRight = 0
    foreach ($row in $Snapshot.Rows)
    {
        $darkLeft += $row.dark_left_count
        $darkCenter += $row.dark_center_count
        $darkRight += $row.dark_right_count
    }

    $frameMatches = ($null -ne $Snapshot.begin_frame) -and
                    ($null -ne $Snapshot.end_frame) -and
                    ($Snapshot.begin_frame -eq $Snapshot.end_frame)
    $complete = $frameMatches -and ($rowCount -eq 24)
    $status = if ($complete)
    {
        "complete"
    }
    elseif ($CloseReason -ne "end_marker")
    {
        $CloseReason
    }
    elseif (($null -ne $Snapshot.begin_frame) -and
            ($null -ne $Snapshot.end_frame) -and
            ($Snapshot.begin_frame -ne $Snapshot.end_frame))
    {
        "frame_mismatch"
    }
    elseif ($rowCount -ne 24)
    {
        "incomplete_row_count"
    }
    else
    {
        "incomplete_metadata"
    }

    $leftDenominator = 67 * $rowCount
    $centerDenominator = 67 * $rowCount
    $rightDenominator = 66 * $rowCount
    $snapshotPayload = (($Snapshot.Rows | ForEach-Object { $_.bits }) -join "`n")
    $snapshotHashBytes = [System.Security.Cryptography.SHA256]::HashData(
        [System.Text.Encoding]::ASCII.GetBytes($snapshotPayload))
    $snapshotHash = [System.BitConverter]::ToString($snapshotHashBytes).Replace('-', '')

    $Destination.Add([pscustomobject]@{
        source_file                 = $Snapshot.source_file
        scenario                    = $Snapshot.scenario
        segment                     = $Snapshot.segment
        snapshot_id                 = $Snapshot.snapshot_id
        begin_source_line           = $Snapshot.begin_source_line
        end_source_line             = $Snapshot.end_source_line
        begin_frame                 = $Snapshot.begin_frame
        end_frame                   = $Snapshot.end_frame
        threshold                   = $Snapshot.threshold
        roi_width                   = $Snapshot.roi_width
        roi_height                  = $Snapshot.roi_height
        roi_y_first                 = $Snapshot.roi_y_first
        roi_y_last                  = $Snapshot.roi_y_last
        row_count                   = $rowCount
        expected_row_count          = 24
        dark_left_count             = $darkLeft
        dark_center_count           = $darkCenter
        dark_right_count            = $darkRight
        d128_left_percent_exact     = if ($leftDenominator -gt 0) { 100.0 * $darkLeft / $leftDenominator } else { $null }
        d128_center_percent_exact   = if ($centerDenominator -gt 0) { 100.0 * $darkCenter / $centerDenominator } else { $null }
        d128_right_percent_exact    = if ($rightDenominator -gt 0) { 100.0 * $darkRight / $rightDenominator } else { $null }
        d128_left_percent_firmware  = if ($leftDenominator -gt 0) { [math]::Floor(100.0 * $darkLeft / $leftDenominator) } else { $null }
        d128_center_percent_firmware = if ($centerDenominator -gt 0) { [math]::Floor(100.0 * $darkCenter / $centerDenominator) } else { $null }
        d128_right_percent_firmware = if ($rightDenominator -gt 0) { [math]::Floor(100.0 * $darkRight / $rightDenominator) } else { $null }
        complete                    = $complete
        status                      = $status
        content_sha256              = $snapshotHash
    })
}

foreach ($inputPath in $InputFiles)
{
    $resolvedPath = (Resolve-Path -LiteralPath $inputPath).Path
    $sourceFile = [System.IO.Path]::GetFileName($resolvedPath)
    $scenario = [System.IO.Path]::GetFileNameWithoutExtension($resolvedPath)
    $lineNumber = 0
    $segment = 1
    $snapshotOrdinal = 0
    $currentSnapshot = $null
    $advanceSegmentAfterLine = $false
    $segmentHasRecords = $false
    $typeCounts = @{
        NAV_METRIC = 0
        BIN_BEGIN = 0
        BIN_ROW = 0
        BIN_END = 0
        EMPTY = 0
        OTHER = 0
    }

    foreach ($rawLine in [System.IO.File]::ReadLines($resolvedPath))
    {
        $lineNumber++

        if ($advanceSegmentAfterLine)
        {
            $segment++
            $snapshotOrdinal = 0
            $segmentHasRecords = $false
            $advanceSegmentAfterLine = $false
        }

        $prefixMatch = [regex]::Match($rawLine, '^\s*(?<prefix>\d+)>\s*')
        $rttPrefix = if ($prefixMatch.Success) { $prefixMatch.Groups['prefix'].Value } else { "" }
        $payload = if ($prefixMatch.Success) { $rawLine.Substring($prefixMatch.Length) } else { $rawLine }

        if ($payload.StartsWith('[SHM0] Ready:') -and $segmentHasRecords)
        {
            if ($null -ne $currentSnapshot)
            {
                Add-SnapshotSummary -Snapshot $currentSnapshot -CloseReason "interrupted_by_restart" -Destination $snapshots
                $currentSnapshot = $null
            }
            $segment++
            $snapshotOrdinal = 0
            $segmentHasRecords = $false
        }

        $recordType = "OTHER"
        $snapshotId = ""
        $navMatch = [regex]::Match(
            $payload,
            '^\[NAV\] frame=(?<frame>\d+) intent=(?<intent>\w+) ipc=(?<ipc>\w+) d128\(L/C/R\)=(?<left>\d+)/(?<center>\d+)/(?<right>\d+)% avg=(?<avg_left>\d+)/(?<avg_center>\d+)/(?<avg_right>\d+)\.$')
        $beginMatch = [regex]::Match(
            $payload,
            '^\[NAV\]\[BIN\] BEGIN frame=(?<frame>\d+) roi=(?<width>\d+)x(?<height>\d+) y=(?<first>\d+)\.\.(?<last>\d+) threshold=(?<threshold>\d+) one=dark\.$')
        $endMatch = [regex]::Match($payload, '^\[NAV\]\[BIN\] END frame=(?<frame>\d+)\.$')

        if ([string]::IsNullOrWhiteSpace($payload))
        {
            $recordType = "EMPTY"
        }
        elseif ($navMatch.Success)
        {
            $recordType = "NAV_METRIC"
            $navMetrics.Add([pscustomobject]@{
                source_file       = $sourceFile
                scenario          = $scenario
                segment           = $segment
                source_line       = $lineNumber
                frame             = [uint32] $navMatch.Groups['frame'].Value
                intent            = $navMatch.Groups['intent'].Value
                ipc_action        = $navMatch.Groups['ipc'].Value
                d128_left         = [uint32] $navMatch.Groups['left'].Value
                d128_center       = [uint32] $navMatch.Groups['center'].Value
                d128_right        = [uint32] $navMatch.Groups['right'].Value
                average_left      = [uint32] $navMatch.Groups['avg_left'].Value
                average_center    = [uint32] $navMatch.Groups['avg_center'].Value
                average_right     = [uint32] $navMatch.Groups['avg_right'].Value
            })
        }
        elseif ($beginMatch.Success)
        {
            $recordType = "BIN_BEGIN"
            if ($null -ne $currentSnapshot)
            {
                Add-SnapshotSummary -Snapshot $currentSnapshot -CloseReason "interrupted_by_new_begin" -Destination $snapshots
            }
            $snapshotOrdinal++
            $snapshotId = "{0}:s{1}:b{2}" -f $scenario, $segment, $snapshotOrdinal
            $currentSnapshot = [pscustomobject]@{
                source_file       = $sourceFile
                scenario          = $scenario
                segment           = $segment
                snapshot_id       = $snapshotId
                begin_source_line = $lineNumber
                end_source_line   = $null
                begin_frame       = [uint32] $beginMatch.Groups['frame'].Value
                end_frame         = $null
                threshold         = [uint32] $beginMatch.Groups['threshold'].Value
                roi_width         = [uint32] $beginMatch.Groups['width'].Value
                roi_height        = [uint32] $beginMatch.Groups['height'].Value
                roi_y_first       = [uint32] $beginMatch.Groups['first'].Value
                roi_y_last        = [uint32] $beginMatch.Groups['last'].Value
                Rows              = [System.Collections.Generic.List[object]]::new()
            }
        }
        elseif ($payload -match '^[01]+$')
        {
            $recordType = "BIN_ROW"
            if ($null -eq $currentSnapshot)
            {
                $snapshotOrdinal++
                $snapshotId = "{0}:s{1}:orphan{2}" -f $scenario, $segment, $snapshotOrdinal
                $currentSnapshot = [pscustomobject]@{
                    source_file       = $sourceFile
                    scenario          = $scenario
                    segment           = $segment
                    snapshot_id       = $snapshotId
                    begin_source_line = $null
                    end_source_line   = $null
                    begin_frame       = $null
                    end_frame         = $null
                    threshold         = $null
                    roi_width         = 200
                    roi_height        = 24
                    roi_y_first       = 88
                    roi_y_last        = 111
                    Rows              = [System.Collections.Generic.List[object]]::new()
                }
            }
            $snapshotId = $currentSnapshot.snapshot_id
            $rowIndex = $currentSnapshot.Rows.Count
            $leftBits = if ($payload.Length -ge 67) { $payload.Substring(0, 67) } else { $payload }
            $centerBits = if ($payload.Length -ge 134) { $payload.Substring(67, 67) } elseif ($payload.Length -gt 67) { $payload.Substring(67) } else { "" }
            $rightBits = if ($payload.Length -gt 134) { $payload.Substring(134) } else { "" }
            $rowRecord = [pscustomobject]@{
                source_file       = $sourceFile
                scenario          = $scenario
                segment           = $segment
                snapshot_id       = $snapshotId
                frame             = $currentSnapshot.begin_frame
                row_index         = $rowIndex
                source_line       = $lineNumber
                bit_count         = $payload.Length
                dark_total_count  = [regex]::Matches($payload, '1').Count
                dark_left_count   = [regex]::Matches($leftBits, '1').Count
                dark_center_count = [regex]::Matches($centerBits, '1').Count
                dark_right_count  = [regex]::Matches($rightBits, '1').Count
                bits              = $payload
            }
            $currentSnapshot.Rows.Add($rowRecord)
            $binaryRows.Add($rowRecord)
        }
        elseif ($endMatch.Success)
        {
            $recordType = "BIN_END"
            if ($null -eq $currentSnapshot)
            {
                $snapshotOrdinal++
                $snapshotId = "{0}:s{1}:endonly{2}" -f $scenario, $segment, $snapshotOrdinal
                $currentSnapshot = [pscustomobject]@{
                    source_file       = $sourceFile
                    scenario          = $scenario
                    segment           = $segment
                    snapshot_id       = $snapshotId
                    begin_source_line = $null
                    end_source_line   = $lineNumber
                    begin_frame       = $null
                    end_frame         = [uint32] $endMatch.Groups['frame'].Value
                    threshold         = $null
                    roi_width         = 200
                    roi_height        = 24
                    roi_y_first       = 88
                    roi_y_last        = 111
                    Rows              = [System.Collections.Generic.List[object]]::new()
                }
            }
            else
            {
                $currentSnapshot.end_source_line = $lineNumber
                $currentSnapshot.end_frame = [uint32] $endMatch.Groups['frame'].Value
            }
            $snapshotId = $currentSnapshot.snapshot_id
            Add-SnapshotSummary -Snapshot $currentSnapshot -CloseReason "end_marker" -Destination $snapshots
            $currentSnapshot = $null
        }

        $typeCounts[$recordType]++
        $rawRecords.Add([pscustomobject]@{
            source_file = $sourceFile
            scenario    = $scenario
            segment     = $segment
            source_line = $lineNumber
            record_type = $recordType
            snapshot_id = $snapshotId
            rtt_prefix  = $rttPrefix
            payload     = $payload
            raw_text    = $rawLine
        })
        $segmentHasRecords = $true

        if ($payload -eq '(Connection lost)')
        {
            if ($null -ne $currentSnapshot)
            {
                Add-SnapshotSummary -Snapshot $currentSnapshot -CloseReason "interrupted_by_connection_loss" -Destination $snapshots
                $currentSnapshot = $null
            }
            $advanceSegmentAfterLine = $true
        }
    }

    if ($null -ne $currentSnapshot)
    {
        Add-SnapshotSummary -Snapshot $currentSnapshot -CloseReason "interrupted_by_end_of_file" -Destination $snapshots
    }

    $fileInfo = Get-Item -LiteralPath $resolvedPath
    $manifest.Add([pscustomobject]@{
        source_file     = $sourceFile
        scenario        = $scenario
        source_path     = $resolvedPath
        sha256          = (Get-FileHash -LiteralPath $resolvedPath -Algorithm SHA256).Hash
        byte_length     = $fileInfo.Length
        line_count      = $lineNumber
        nav_metric      = $typeCounts.NAV_METRIC
        bin_begin       = $typeCounts.BIN_BEGIN
        bin_row         = $typeCounts.BIN_ROW
        bin_end         = $typeCounts.BIN_END
        empty           = $typeCounts.EMPTY
        other           = $typeCounts.OTHER
        segment_count   = $segment
    })
}

foreach ($group in ($snapshots |
                    Where-Object { $_.complete } |
                    Group-Object content_sha256 |
                    Where-Object { $_.Count -gt 1 }))
{
    foreach ($snapshot in $group.Group)
    {
        $snapshotDuplicates.Add([pscustomobject]@{
            content_sha256 = $group.Name
            duplicate_count = $group.Count
            source_file = $snapshot.source_file
            scenario = $snapshot.scenario
            segment = $snapshot.segment
            snapshot_id = $snapshot.snapshot_id
            frame = $snapshot.begin_frame
        })
    }
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$rawRecords | Export-Csv -LiteralPath (Join-Path $OutputDirectory "raw_records.csv") -NoTypeInformation -Encoding utf8
$navMetrics | Export-Csv -LiteralPath (Join-Path $OutputDirectory "nav_metrics.csv") -NoTypeInformation -Encoding utf8
$binaryRows | Export-Csv -LiteralPath (Join-Path $OutputDirectory "binary_rows.csv") -NoTypeInformation -Encoding utf8
$snapshots | Export-Csv -LiteralPath (Join-Path $OutputDirectory "binary_snapshots.csv") -NoTypeInformation -Encoding utf8
$snapshotDuplicates | Export-Csv -LiteralPath (Join-Path $OutputDirectory "snapshot_duplicates.csv") -NoTypeInformation -Encoding utf8
$manifest | Export-Csv -LiteralPath (Join-Path $OutputDirectory "manifest.csv") -NoTypeInformation -Encoding utf8

Write-Output ("Preprocessed {0} files into {1}" -f $InputFiles.Count, (Resolve-Path -LiteralPath $OutputDirectory).Path)
Write-Output ("Records={0}, nav_metrics={1}, binary_rows={2}, snapshots={3}" -f $rawRecords.Count, $navMetrics.Count, $binaryRows.Count, $snapshots.Count)
