param(
    [string]$OptionsPath = "",
    [string]$StandardSettingsPath = "",
    [int]$TargetDpi = 800,
    [int]$TargetWindowsSpeed = 0,
    [double]$TargetSensitivity = -1.0,
    [double]$PreferredStandardSensitivity = -1.0,
    [int]$CurrentDpi = 0,
    [switch]$PreferLowestPixelSkipping,
    [ValidateSet("pixel-perfect", "target-mapped")]
    [string]$RecommendationMode = "pixel-perfect",
    [ValidateSet("auto", "manual")]
    [string]$InputMode = "auto",
    [int]$ManualCurrentWindowsSpeed = 0,
    [int]$RecommendationChoice = 1,
    [object]$LowestSkipChoiceOne = $true,
    [object]$IncludeCursorInRanking = $true,
    [object]$PreferHigherDpi = $false,
    [double]$MaxRecommendedPixelSkipping = 50.0,
    [string]$SensitivityListPath = "",
    [string]$SensitivityListUrl = "https://gist.githubusercontent.com/ExeRSolver/cd8e89256a5f51ee4e32ba9df2db748f/raw/84b704b7d8f0e5e17d364368d351ad3fa39bb3f3/boatEyeSensitivitiesv1_16.txt",
    [switch]$Json,
    [switch]$Apply,
    [switch]$PreferManualTargets,
    [switch]$RestoreLatestBackup,
    [string]$RestoreBackupPath = "",
    [switch]$ApplyVisualEffectsOnly,
    [switch]$ApplyVisualEffects,
    [double]$TargetScreenEffectScale = 0.0,
    [double]$TargetFovEffectScale = 0.1,
    [switch]$NoPopup,
    [switch]$NoDisableMouseAccel,
    [switch]$NoEnableRawInput
)

$ErrorActionPreference = "Stop"
$script:EmitJsonOnly = $Json.IsPresent

function Write-Info([string]$message = "") {
    if (-not $script:EmitJsonOnly) { Write-Host $message }
}

function Emit-JsonAndExit([object]$payload, [int]$code = 0) {
    Write-Output ($payload | ConvertTo-Json -Depth 10 -Compress)
    exit $code
}

function Clamp-Double([double]$v, [double]$min, [double]$max) {
    if ($v -lt $min) { return $min }
    if ($v -gt $max) { return $max }
    return $v
}

function Format-DoubleInvariant([double]$v, [string]$fmt = "0.########") {
    return $v.ToString($fmt, [System.Globalization.CultureInfo]::InvariantCulture)
}

function Convert-ToBool([object]$value, [bool]$defaultValue, [string]$name) {
    if ($null -eq $value) { return $defaultValue }
    if ($value -is [bool]) { return [bool]$value }

    if ($value -is [byte] -or $value -is [sbyte] -or $value -is [int16] -or $value -is [uint16] -or
        $value -is [int32] -or $value -is [uint32] -or $value -is [int64] -or $value -is [uint64] -or
        $value -is [single] -or $value -is [double] -or $value -is [decimal]) {
        return ([double]$value) -ne 0.0
    }

    $text = "$value".Trim().ToLowerInvariant()
    if ([string]::IsNullOrWhiteSpace($text)) { return $defaultValue }
    switch ($text) {
        "1" { return $true }
        "0" { return $false }
        "true" { return $true }
        "false" { return $false }
        "$true" { return $true }
        "$false" { return $false }
        "yes" { return $true }
        "no" { return $false }
        "on" { return $true }
        "off" { return $false }
        default { throw "Invalid boolean value for ${name}: '$($value)' (use true/false or 1/0)." }
    }
}

function Get-NbbRegistryPath {
    return "HKCU:\\Software\\JavaSoft\\Prefs\\ninjabrainbot"
}

function Get-NbbRegistryDouble([string]$name) {
    try {
        $path = Get-NbbRegistryPath
        $item = Get-ItemProperty -Path $path -Name $name -ErrorAction Stop
        if ($null -eq $item) { return $null }
        $raw = $item.$name
        if ($null -eq $raw) { return $null }
        [double]$parsed = 0.0
        if ([double]::TryParse("$raw", [System.Globalization.NumberStyles]::Float,
                [System.Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
            return [double]$parsed
        }
        return $null
    } catch {
        return $null
    }
}

function Set-NbbRegistryDouble([string]$name, [double]$value) {
    $path = Get-NbbRegistryPath
    if (-not (Test-Path -LiteralPath $path)) {
        New-Item -Path $path -Force | Out-Null
    }
    $txt = Format-DoubleInvariant (Clamp-Double $value 0.0 1.0)
    Set-ItemProperty -Path $path -Name $name -Value $txt -Type String
}

function Get-CursorSpeedMultipliers {
    # Matches https://priffin.github.io/Pixel-Perfect-Tools/calc.html exactly.
    return [double[]](
        0.03125, 0.0625, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0,
        1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 3.25, 3.5
    )
}

function Resolve-SensitivityListPath([string]$requestedPath) {
    $candidates = New-Object System.Collections.Generic.List[string]
    $defaultFile = "boatEyeSensitivitiesv1_16.txt"

    if (-not [string]::IsNullOrWhiteSpace($requestedPath)) {
        if (Test-Path -LiteralPath $requestedPath -PathType Container) {
            $candidates.Add((Join-Path $requestedPath $defaultFile)) | Out-Null
        } else {
            $candidates.Add($requestedPath) | Out-Null
        }
    }

    # Bundled canonical source (preferred).
    if ($PSScriptRoot) {
        $candidates.Add((Join-Path $PSScriptRoot "resources\\boat_eye_senses\\$defaultFile")) | Out-Null
        $candidates.Add((Join-Path $PSScriptRoot "resources\\$defaultFile")) | Out-Null
    }

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            try {
                return (Get-Item -LiteralPath $candidate -ErrorAction Stop).FullName
            } catch {
            }
        }
    }

    return ""
}

function Parse-BoatEyeSensitivitiesRaw([string]$raw) {
    $lines = ($raw -split "(?:`r`n|`n|`r)")
    $vals = New-Object 'System.Collections.Generic.List[double]'
    $lineCount = 0
    $parsedCount = 0
    $acceptedCount = 0
    $rxFirstNumber = '^\s*([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)'

    for ($i = 0; $i -lt $lines.Count; $i++) {
        $lineCount++
        $line = ($lines[$i])
        if ($null -eq $line) { continue }
        # Remove UTF-8 BOM if present on first line.
        if ($i -eq 0 -and $line.Length -gt 0 -and [int][char]$line[0] -eq 0xFEFF) {
            $line = $line.Substring(1)
        }
        $line = $line.Trim()
        if (-not $line) { continue }

        # Parse first-column number only; ignore headers/comments and trailing columns.
        $m = [System.Text.RegularExpressions.Regex]::Match($line, $rxFirstNumber)
        if (-not $m.Success) { continue }
        $token = $m.Groups[1].Value
        $parsedCount++

        [double]$parsed = 0.0
        $ok = [double]::TryParse($token, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)
        if (-not $ok -and $token.Contains(",")) {
            # Fallback: handle comma decimal formatting.
            $tokenAlt = $token.Replace(",", ".")
            $ok = [double]::TryParse($tokenAlt, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)
        }
        if (-not $ok) { continue }

        if ($parsed -ge 0.0 -and $parsed -le 1.0) {
            $vals.Add($parsed) | Out-Null
            $acceptedCount++
        }
    }

    if ($vals.Count -lt 2) {
        throw "Boat-eye sensitivity list parse failed (not enough values)."
    }

    $sorted = [double[]]($vals | Sort-Object -Unique)
    return [PSCustomObject]@{
        Values = $sorted
        Stats = [PSCustomObject]@{
            lineCount = $lineCount
            parsedFirstColumnCount = $parsedCount
            acceptedRangeCount = $acceptedCount
            uniqueCount = $sorted.Count
        }
    }
}

function Get-BoatEyeSensitivities([string]$localPath, [string]$url) {
    $resolvedLocalPath = Resolve-SensitivityListPath $localPath
    if (-not [string]::IsNullOrWhiteSpace($resolvedLocalPath)) {
        $rawLocal = Get-Content -LiteralPath $resolvedLocalPath -Raw -ErrorAction Stop
        $parsedLocal = Parse-BoatEyeSensitivitiesRaw $rawLocal
        return [PSCustomObject]@{
            Values = $parsedLocal.Values
            ParseStats = $parsedLocal.Stats
            Source = $resolvedLocalPath
            SourceType = "bundled-file"
        }
    }

    $cacheDir = Join-Path $env:LOCALAPPDATA "Toolscreen\\cache"
    $cachePath = Join-Path $cacheDir "boatEyeSensitivitiesv1_16.txt"
    New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null

    $raw = $null
    try {
        $resp = Invoke-WebRequest -UseBasicParsing -Uri $url -TimeoutSec 10
        if ($resp -and $resp.Content) {
            $raw = $resp.Content
            $raw | Out-File -LiteralPath $cachePath -Encoding utf8
        }
    } catch {
        if (Test-Path -LiteralPath $cachePath) {
            $raw = Get-Content -LiteralPath $cachePath -Raw -ErrorAction Stop
        } else {
            throw "Failed to fetch boat-eye sensitivity list and no local cache exists."
        }
    }
    $parsedRemote = Parse-BoatEyeSensitivitiesRaw $raw
    return [PSCustomObject]@{
        Values = $parsedRemote.Values
        ParseStats = $parsedRemote.Stats
        Source = $cachePath
        SourceType = "url-or-cache"
    }
}

function Compute-SensitivityFromMultiplier([double]$sensMultiplier) {
    $v = (([Math]::Pow(($sensMultiplier / 8.0), (1.0 / 3.0)) - 0.2) / 0.6)
    return $v
}

function Compute-SensitivityMultiplier([double]$sens) {
    return ([Math]::Pow(($sens * 0.6 + 0.2), 3) * 8.0)
}

function Resolve-ClosestBoatEyeSensitivity([double]$rawSensitivity, [double[]]$sensList) {
    if ($sensList.Count -lt 2) {
        throw "Need at least two sensitivity entries."
    }

    $first = [double]$sensList[0]
    $last = [double]$sensList[$sensList.Count - 1]

    if ($rawSensitivity -le $first) {
        $minInc = 1.2 * [Math]::Pow((0.2 + 0.6 * $first), 3)
        return [PSCustomObject]@{
            Primary = $first
            Secondary = $null
            PixelSkipping = ($minInc / 0.00220031449588)
        }
    }
    if ($rawSensitivity -ge $last) {
        $minInc = 1.2 * [Math]::Pow((0.2 + 0.6 * $last), 3)
        return [PSCustomObject]@{
            Primary = $last
            Secondary = $null
            PixelSkipping = ($minInc / 0.00220031449588)
        }
    }

    for ($i = 0; $i -lt ($sensList.Count - 1); $i++) {
        $a = [double]$sensList[$i]
        $b = [double]$sensList[$i + 1]
        $d1 = $rawSensitivity - $a
        $d2 = $rawSensitivity - $b

        if ([Math]::Abs($d1) -lt 1e-12) {
            $minInc = 1.2 * [Math]::Pow((0.2 + 0.6 * $a), 3)
            return [PSCustomObject]@{
                Primary = $a
                Secondary = $b
                PixelSkipping = ($minInc / 0.00220031449588)
            }
        }
        if (($d1 * $d2) -lt 0) {
            $minInc = 1.2 * [Math]::Pow((0.2 + 0.6 * $a), 3)
            return [PSCustomObject]@{
                Primary = $a
                Secondary = $b
                PixelSkipping = ($minInc / 0.00220031449588)
            }
        }
    }

    $nearest = $sensList | Sort-Object { [Math]::Abs($_ - $rawSensitivity) } | Select-Object -First 1
    $minInc = 1.2 * [Math]::Pow((0.2 + 0.6 * $nearest), 3)
    return [PSCustomObject]@{
        Primary = [double]$nearest
        Secondary = $null
        PixelSkipping = ($minInc / 0.00220031449588)
    }
}

function Build-RecommendationFromRawSensitivity([double]$rawSensitivity, [double]$targetDpi, [int]$targetCursorSpeed, [string]$sourceMode,
                                                 [double[]]$sensList) {
    $snap = Resolve-ClosestBoatEyeSensitivity -rawSensitivity $rawSensitivity -sensList $sensList

    $primary = [double]$snap.Primary
    $secondary = $null
    if ($null -ne $snap.Secondary) {
        $secondary = [double]$snap.Secondary
    }

    $selected = $primary
    if ($null -ne $secondary) {
        $dPrimary = [Math]::Abs($rawSensitivity - $primary)
        $dSecondary = [Math]::Abs($rawSensitivity - $secondary)
        if ($dSecondary -lt $dPrimary) {
            $selected = $secondary
        }
    }

    return [PSCustomObject]@{
        Source = $sourceMode
        TargetDpiRaw = [double]$targetDpi
        TargetDpiRounded = [int][Math]::Round([double]$targetDpi)
        TargetCursorSpeed = [int](Clamp-Double $targetCursorSpeed 1 20)
        RawSensitivity = [Math]::Round([double]$rawSensitivity, 8)
        PrimarySensitivity = [Math]::Round($primary, 8)
        SecondarySensitivity = if ($null -ne $secondary) { [Math]::Round($secondary, 8) } else { $null }
        SelectedSensitivity = [Math]::Round($selected, 8)
        EstimatedPixelSkipping = [Math]::Round([double]$snap.PixelSkipping, 2)
    }
}

function Compute-RawSensitivityForTarget([double]$currentSensitivity, [int]$currentDpi, [int]$currentCursorSpeed, [int]$targetDpi,
                                         [int]$targetCursorSpeed) {
    $m = Get-CursorSpeedMultipliers
    $currentCursorSpeed = [int](Clamp-Double $currentCursorSpeed 1 20)
    $targetCursorSpeed = [int](Clamp-Double $targetCursorSpeed 1 20)

    $origMult = Compute-SensitivityMultiplier $currentSensitivity
    $factor = ($currentDpi * $m[$currentCursorSpeed - 1]) / ($targetDpi * $m[$targetCursorSpeed - 1])
    $newSensMult = $origMult * $factor
    return (Compute-SensitivityFromMultiplier $newSensMult)
}

function Compute-PixelPerfectRecommendationForCursorSpeed([int]$currentDpi, [double]$currentSensitivity, [int]$currentCursorSpeed,
                                                          [int]$targetCursorSpeed, [double[]]$sensList) {
    $m = Get-CursorSpeedMultipliers
    $currentCursorSpeed = [int](Clamp-Double $currentCursorSpeed 1 20)
    $targetCursorSpeed = [int](Clamp-Double $targetCursorSpeed 1 20)

    $origMult = Compute-SensitivityMultiplier $currentSensitivity
    $ratio = $m[$targetCursorSpeed - 1] / $m[$currentCursorSpeed - 1]
    $newDpi = $currentDpi / $ratio
    $newSensMult = $origMult * $ratio
    $rawSens = Compute-SensitivityFromMultiplier $newSensMult

    return Build-RecommendationFromRawSensitivity -rawSensitivity $rawSens -targetDpi $newDpi -targetCursorSpeed $targetCursorSpeed `
        -sourceMode "pixel-perfect-preferred-speed" -sensList $sensList
}

function Compute-PixelPerfectAutoRecommendation([int]$currentDpi, [double]$currentSensitivity, [int]$currentCursorSpeed,
                                                [double[]]$sensList, [bool]$preferLowestPixelSkipping, [int]$targetCursorConstraint,
                                                [int]$recommendationChoice, [bool]$lowestSkipChoiceOne, [bool]$includeCursorInRanking,
                                                [bool]$preferHigherDpi, [double]$maxRecommendedPixelSkipping) {
    $m = Get-CursorSpeedMultipliers
    $currentCursorSpeed = [int](Clamp-Double $currentCursorSpeed 1 20)
    $origMult = Compute-SensitivityMultiplier $currentSensitivity

    $bestClosest = $null
    $bestLowest = $null
    $candidates = New-Object System.Collections.Generic.List[object]
    $candidatesFiltered = New-Object System.Collections.Generic.List[object]

    $cursorPreference = [int](Clamp-Double $targetCursorConstraint 0 20)
    $speedReference = if ($cursorPreference -gt 0) { $cursorPreference } else { $currentCursorSpeed }
    $maxSkipFilter = [double](Clamp-Double $maxRecommendedPixelSkipping 0.1 5000.0)

    for ($speed = 1; $speed -le 20; $speed++) {
        $ratio = $m[$speed - 1] / $m[$currentCursorSpeed - 1]
        $newDpi = $currentDpi / $ratio
        $newSensMult = $origMult * $ratio
        $rawSens = Compute-SensitivityFromMultiplier $newSensMult
        if ($rawSens -le 0) { continue }

        $rec = Build-RecommendationFromRawSensitivity -rawSensitivity $rawSens -targetDpi $newDpi -targetCursorSpeed $speed `
            -sourceMode "pixel-perfect-auto" -sensList $sensList

        $speedDelta = [Math]::Abs($speed - $speedReference)
        $dpiDeltaRatio = [Math]::Abs($newDpi - $currentDpi) / [Math]::Max([double]$currentDpi, 1.0)
        $sensDelta = [Math]::Abs($rawSens - $currentSensitivity)
        $speedScore = if ($includeCursorInRanking) { $speedDelta * 5.0 } else { 0.0 }
        $score = $speedScore + ($dpiDeltaRatio * 3.0) + $sensDelta

        $pixelSkipping = [double]$rec.EstimatedPixelSkipping
        $candidate = [PSCustomObject]@{
            Rec = $rec
            Score = $score
            PixelSkipping = $pixelSkipping
            SensitivityDelta = [double]$sensDelta
            SpeedDelta = [double]$speedDelta
            DpiDeltaRatio = [double]$dpiDeltaRatio
        }
        $candidates.Add($candidate) | Out-Null

        if ($pixelSkipping -le ($maxSkipFilter + 1e-9)) {
            $candidatesFiltered.Add($candidate) | Out-Null
            if ($null -eq $bestClosest -or $score -lt $bestClosest.Score) {
                $bestClosest = $candidate
            }
            if ($null -eq $bestLowest -or
                $pixelSkipping -lt ($bestLowest.PixelSkipping - 1e-9) -or
                ([Math]::Abs($pixelSkipping - $bestLowest.PixelSkipping) -le 1e-9 -and $score -lt $bestLowest.Score)) {
                $bestLowest = $candidate
            }
        }
    }

    $skipFilterIgnored = $false
    if ($candidatesFiltered.Count -eq 0) {
        $skipFilterIgnored = $true
        foreach ($candidate in $candidates) { $candidatesFiltered.Add($candidate) | Out-Null }
        foreach ($candidate in $candidatesFiltered) {
            if ($null -eq $bestClosest -or $candidate.Score -lt $bestClosest.Score) {
                $bestClosest = $candidate
            }
            if ($null -eq $bestLowest -or
                $candidate.PixelSkipping -lt ($bestLowest.PixelSkipping - 1e-9) -or
                ([Math]::Abs($candidate.PixelSkipping - $bestLowest.PixelSkipping) -le 1e-9 -and $candidate.Score -lt $bestLowest.Score)) {
                $bestLowest = $candidate
            }
        }
    }

    if ($null -eq $bestClosest -or $null -eq $bestLowest) {
        throw "Could not derive a valid pixel-perfect recommendation."
    }

    foreach ($candidate in $candidatesFiltered) {
        $skipRatio = [Math]::Max(1e-9, [double]$candidate.PixelSkipping / [Math]::Max([double]$bestLowest.PixelSkipping, 1e-9))
        # Balanced score: retain low-skip pressure, but strongly favor sensitivity + cursor similarity.
        $skipPenalty = [Math]::Log($skipRatio + 1.0)
        $speedPenalty = if ($includeCursorInRanking) { [double]$candidate.SpeedDelta * 1.25 } else { 0.0 }
        $dpiLowerPenalty = 0.0
        if ($preferHigherDpi -and [double]$candidate.Rec.TargetDpiRounded -lt [double]$currentDpi) {
            $dpiLowerPenalty = (([double]$currentDpi - [double]$candidate.Rec.TargetDpiRounded) / [Math]::Max([double]$currentDpi, 1.0)) * 1.2
        }
        $exploreScore = ($skipPenalty * 3.5) + ([double]$candidate.SensitivityDelta * 60.0) + $speedPenalty + $dpiLowerPenalty
        $candidate | Add-Member -NotePropertyName ExploreScore -NotePropertyValue ([double]$exploreScore) -Force
        $candidate | Add-Member -NotePropertyName DpiLowerPenalty -NotePropertyValue ([double]$dpiLowerPenalty) -Force
    }

    $chosen = $bestLowest
    $selectionPolicy = "lowest-skipping"
    if (-not $lowestSkipChoiceOne -and ($cursorPreference -gt 0 -or $preferHigherDpi -or (-not $includeCursorInRanking))) {
        $sortSpec = @(
            @{ Expression = "ExploreScore"; Ascending = $true },
            @{ Expression = "PixelSkipping"; Ascending = $true },
            @{ Expression = "SensitivityDelta"; Ascending = $true }
        )
        if ($includeCursorInRanking) {
            $sortSpec += @{ Expression = "SpeedDelta"; Ascending = $true }
        }
        if ($preferHigherDpi) {
            $sortSpec += @{ Expression = { [double]$_.Rec.TargetDpiRounded }; Descending = $true }
        }
        $bestExplore = @($candidatesFiltered | Sort-Object -Property $sortSpec) | Select-Object -First 1
        if ($null -ne $bestExplore -and $bestExplore.Count -gt 0) {
            $chosen = $bestExplore[0]
            if ([Math]::Abs([double]$chosen.PixelSkipping - [double]$bestLowest.PixelSkipping) -gt 1e-9 -or
                [int]$chosen.Rec.TargetCursorSpeed -ne [int]$bestLowest.Rec.TargetCursorSpeed -or
                [Math]::Abs([double]$chosen.Rec.SelectedSensitivity - [double]$bestLowest.Rec.SelectedSensitivity) -gt 1e-9) {
                $selectionPolicy = "balanced-similarity"
            }
        }
    }

    $chosen.Rec | Add-Member -NotePropertyName SelectionScore -NotePropertyValue ([Math]::Round([double]$chosen.Score, 6)) -Force
    $chosen.Rec | Add-Member -NotePropertyName SelectionPolicy -NotePropertyValue $selectionPolicy -Force
    $chosen.Rec | Add-Member -NotePropertyName CursorSpeedPreference -NotePropertyValue $cursorPreference -Force
    $chosen.Rec | Add-Member -NotePropertyName CursorSpeedReference -NotePropertyValue $speedReference -Force
    $chosen.Rec | Add-Member -NotePropertyName IncludeCursorInRanking -NotePropertyValue $includeCursorInRanking -Force
    $chosen.Rec | Add-Member -NotePropertyName PreferHigherDpi -NotePropertyValue $preferHigherDpi -Force
    $chosen.Rec | Add-Member -NotePropertyName MaxRecommendedPixelSkipping -NotePropertyValue ([Math]::Round([double]$maxSkipFilter, 2)) -Force
    $chosen.Rec | Add-Member -NotePropertyName SkipFilterIgnored -NotePropertyValue $skipFilterIgnored -Force
    $chosen.Rec | Add-Member -NotePropertyName CursorSoftSkipTolerance -NotePropertyValue 0.0 -Force
    $chosen.Rec | Add-Member -NotePropertyName SpeedDeltaFromPreference -NotePropertyValue ([int][Math]::Abs([int]$chosen.Rec.TargetCursorSpeed - [int]$speedReference)) -Force
    $chosen.Rec | Add-Member -NotePropertyName ClosestFeelPixelSkipping -NotePropertyValue ([Math]::Round([double]$bestClosest.PixelSkipping, 2)) -Force
    $chosen.Rec | Add-Member -NotePropertyName LowestPixelSkipping -NotePropertyValue ([Math]::Round([double]$bestLowest.PixelSkipping, 2)) -Force
    $chosen.Rec | Add-Member -NotePropertyName LowestSkipChoiceOne -NotePropertyValue $lowestSkipChoiceOne -Force

    $skipTolerance = 0.0
    $ordered = New-Object System.Collections.Generic.List[object]
    $ordered.Add($chosen) | Out-Null
    $sortedByExplore = @($candidatesFiltered | Sort-Object -Property @{Expression = "ExploreScore"; Ascending = $true},
                                                   @{Expression = "PixelSkipping"; Ascending = $true},
                                                   @{Expression = "SensitivityDelta"; Ascending = $true},
                                                   @{Expression = "SpeedDelta"; Ascending = $true},
                                                   @{Expression = { [double]$_.Rec.TargetDpiRounded }; Descending = $true})
    foreach ($candidate in $sortedByExplore) {
        if ([Math]::Abs([double]$candidate.PixelSkipping - [double]$chosen.PixelSkipping) -le 1e-9 -and
            [int]$candidate.Rec.TargetCursorSpeed -eq [int]$chosen.Rec.TargetCursorSpeed -and
            [int]$candidate.Rec.TargetDpiRounded -eq [int]$chosen.Rec.TargetDpiRounded -and
            [Math]::Abs([double]$candidate.Rec.SelectedSensitivity - [double]$chosen.Rec.SelectedSensitivity) -le 1e-9) {
            continue
        }
        $ordered.Add($candidate) | Out-Null
    }
    $choiceMax = [Math]::Min(12, $ordered.Count)
    $choiceSelected = [int](Clamp-Double $recommendationChoice 1 $choiceMax)
    $choiceIndex = $choiceSelected - 1
    $chosen = $ordered[$choiceIndex]
    if ($choiceSelected -gt 1) {
        $selectionPolicy = "ranked-choice"
    }

    $choiceList = New-Object System.Collections.Generic.List[object]
    for ($i = 0; $i -lt $choiceMax; $i++) {
        $entry = $ordered[$i]
        $choiceList.Add([PSCustomObject]@{
            Rank = $i + 1
            TargetDpiRounded = [int]$entry.Rec.TargetDpiRounded
            TargetCursorSpeed = [int]$entry.Rec.TargetCursorSpeed
            SelectedSensitivity = [Math]::Round([double]$entry.Rec.SelectedSensitivity, 8)
            EstimatedPixelSkipping = [Math]::Round([double]$entry.PixelSkipping, 2)
            SpeedDeltaFromPreference = [int][Math]::Abs([int]$entry.Rec.TargetCursorSpeed - [int]$speedReference)
            SensitivityDelta = [Math]::Round([double]$entry.SensitivityDelta, 8)
            SensitivityDeltaPercent = [Math]::Round([double]$entry.SensitivityDelta * 200.0, 2)
            ExploreScore = [Math]::Round([double]$entry.ExploreScore, 6)
        }) | Out-Null
    }

    $chosen.Rec | Add-Member -NotePropertyName SelectionPolicy -NotePropertyValue $selectionPolicy -Force
    $chosen.Rec | Add-Member -NotePropertyName RecommendationChoice -NotePropertyValue $choiceSelected -Force
    $chosen.Rec | Add-Member -NotePropertyName RecommendationChoiceMax -NotePropertyValue $choiceMax -Force
    $chosen.Rec | Add-Member -NotePropertyName CandidateChoices -NotePropertyValue $choiceList -Force
    $chosen.Rec | Add-Member -NotePropertyName CursorSpeedPreference -NotePropertyValue $cursorPreference -Force
    $chosen.Rec | Add-Member -NotePropertyName CursorSpeedReference -NotePropertyValue $speedReference -Force
    $chosen.Rec | Add-Member -NotePropertyName CursorSoftSkipTolerance -NotePropertyValue ([Math]::Round([double]$skipTolerance, 2)) -Force
    $chosen.Rec | Add-Member -NotePropertyName SpeedDeltaFromPreference -NotePropertyValue ([int][Math]::Abs([int]$chosen.Rec.TargetCursorSpeed - [int]$speedReference)) -Force
    $chosen.Rec | Add-Member -NotePropertyName ClosestFeelPixelSkipping -NotePropertyValue ([Math]::Round([double]$bestClosest.PixelSkipping, 2)) -Force
    $chosen.Rec | Add-Member -NotePropertyName LowestPixelSkipping -NotePropertyValue ([Math]::Round([double]$bestLowest.PixelSkipping, 2)) -Force
    $chosen.Rec | Add-Member -NotePropertyName LowestSkipChoiceOne -NotePropertyValue $lowestSkipChoiceOne -Force
    return $chosen.Rec
}

function Resolve-OptionsPath([string]$requestedPath) {
    $candidates = New-Object System.Collections.Generic.List[string]

    if (-not [string]::IsNullOrWhiteSpace($requestedPath)) {
        $candidates.Add($requestedPath)
    }
    if ($env:INST_MC_DIR) {
        $candidates.Add((Join-Path $env:INST_MC_DIR "options.txt"))
    }
    if ($env:INST_DIR) {
        $candidates.Add((Join-Path $env:INST_DIR ".minecraft\\options.txt"))
        $candidates.Add((Join-Path $env:INST_DIR "options.txt"))
    }

    $userProfile = [Environment]::GetFolderPath("UserProfile")
    $appData = [Environment]::GetFolderPath("ApplicationData")
    if ($userProfile) {
        $candidates.Add((Join-Path $userProfile ".minecraft\\options.txt"))
        $candidates.Add((Join-Path $userProfile "AppData\\Roaming\\.minecraft\\options.txt"))
        $candidates.Add((Join-Path $userProfile "Desktop\\msr\\MultiMC\\instances\\MCSRRanked-Windows-1.16.1-All\\.minecraft\\options.txt"))
        $mmcInstances = Join-Path $userProfile "Desktop\\msr\\MultiMC\\instances"
        if (Test-Path -LiteralPath $mmcInstances) {
            Get-ChildItem -LiteralPath $mmcInstances -Directory -ErrorAction SilentlyContinue | ForEach-Object {
                $candidates.Add((Join-Path $_.FullName ".minecraft\\options.txt"))
            }
        }
    }
    if ($appData) {
        $candidates.Add((Join-Path $appData ".minecraft\\options.txt"))
    }

    $existing = @()
    foreach ($path in $candidates) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            try {
                $item = Get-Item -LiteralPath $path -ErrorAction Stop
                $existing += [PSCustomObject]@{
                    Path = $item.FullName
                    LastWrite = $item.LastWriteTimeUtc
                }
            } catch {
            }
        }
    }

    if ($existing.Count -eq 0) {
        throw "Could not find options.txt. Pass -OptionsPath explicitly."
    }

    $picked = $existing | Sort-Object LastWrite -Descending | Select-Object -First 1
    return $picked.Path
}

function Parse-OptionsFile([string]$path) {
    $lines = Get-Content -LiteralPath $path -ErrorAction Stop
    $kv = @{}
    $list = New-Object 'System.Collections.Generic.List[string]'
    foreach ($line in $lines) {
        $list.Add([string]$line) | Out-Null
        if ($line -match "^(?<k>[^:]+):(?<v>.*)$") {
            $kv[$Matches.k] = $Matches.v
        }
    }
    return [PSCustomObject]@{
        Lines = $list
        Map = $kv
    }
}

function Set-OptionValue([System.Collections.Generic.List[string]]$lines, [string]$key, [string]$value) {
    $prefix = "${key}:"
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].StartsWith($prefix, [System.StringComparison]::Ordinal)) {
            $lines[$i] = "${prefix}${value}"
            return
        }
    }
    $lines.Add("${prefix}${value}") | Out-Null
}

function Write-OptionsFile([string]$path, [System.Collections.Generic.List[string]]$lines) {
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllLines($path, $lines, $utf8NoBom)
}

function Resolve-StandardSettingsPath([string]$requestedPath, [string]$resolvedOptionsPath) {
    $candidates = New-Object System.Collections.Generic.List[string]

    if (-not [string]::IsNullOrWhiteSpace($requestedPath)) {
        if (Test-Path -LiteralPath $requestedPath -PathType Container) {
            $candidates.Add((Join-Path $requestedPath "standardsettings.json")) | Out-Null
        } else {
            $candidates.Add($requestedPath) | Out-Null
        }
    }

    if ($env:INST_MC_DIR) {
        $candidates.Add((Join-Path $env:INST_MC_DIR "config\\mcsr\\standardsettings.json")) | Out-Null
        $candidates.Add((Join-Path $env:INST_MC_DIR "config\\standardsettings.json")) | Out-Null
    }
    if ($env:INST_DIR) {
        $candidates.Add((Join-Path $env:INST_DIR ".minecraft\\config\\mcsr\\standardsettings.json")) | Out-Null
        $candidates.Add((Join-Path $env:INST_DIR ".minecraft\\config\\standardsettings.json")) | Out-Null
    }

    if (-not [string]::IsNullOrWhiteSpace($resolvedOptionsPath)) {
        try {
            $optDir = Split-Path -Parent $resolvedOptionsPath
            if (-not [string]::IsNullOrWhiteSpace($optDir)) {
                $candidates.Add((Join-Path $optDir "config\\mcsr\\standardsettings.json")) | Out-Null
                $candidates.Add((Join-Path $optDir "config\\standardsettings.json")) | Out-Null
            }
        } catch {
        }
    }

    $userProfile = [Environment]::GetFolderPath("UserProfile")
    if ($userProfile) {
        $candidates.Add((Join-Path $userProfile ".minecraft\\config\\mcsr\\standardsettings.json")) | Out-Null
        $candidates.Add((Join-Path $userProfile "AppData\\Roaming\\.minecraft\\config\\mcsr\\standardsettings.json")) | Out-Null
    }

    $existing = @()
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            try {
                $item = Get-Item -LiteralPath $candidate -ErrorAction Stop
                $existing += [PSCustomObject]@{
                    Path = $item.FullName
                    LastWrite = $item.LastWriteTimeUtc
                }
            } catch {
            }
        }
    }

    if ($existing.Count -eq 0) { return "" }
    $picked = $existing | Sort-Object LastWrite -Descending | Select-Object -First 1
    return $picked.Path
}

function Parse-StandardSettingsFile([string]$path) {
    if ([string]::IsNullOrWhiteSpace($path)) { return $null }
    $raw = Get-Content -LiteralPath $path -Raw -ErrorAction Stop
    $obj = $raw | ConvertFrom-Json -ErrorAction Stop
    [double]$sens = 0.0
    $hasSens = $false
    if ($null -ne $obj -and $null -ne $obj.PSObject -and $obj.PSObject.Properties.Match("mouseSensitivity").Count -gt 0) {
        $sensValue = $obj.mouseSensitivity
        if ($null -ne $sensValue) {
            $sensText = "$sensValue"
            $hasSens = [double]::TryParse($sensText, [System.Globalization.NumberStyles]::Float,
                                          [System.Globalization.CultureInfo]::InvariantCulture, [ref]$sens)
            if (-not $hasSens -and $sensText.Contains(",")) {
                $hasSens = [double]::TryParse($sensText.Replace(",", "."), [System.Globalization.NumberStyles]::Float,
                                              [System.Globalization.CultureInfo]::InvariantCulture, [ref]$sens)
            }
        }
    }
    return [PSCustomObject]@{
        Path = $path
        Json = $obj
        Raw = $raw
        HasSensitivity = $hasSens
        Sensitivity = if ($hasSens) { [double]$sens } else { $null }
    }
}

function Set-StandardSettingsSensitivity([object]$jsonObj, [double]$value) {
    if ($null -eq $jsonObj) { return $null }
    if ($null -ne $jsonObj.PSObject -and $jsonObj.PSObject.Properties.Match("mouseSensitivity").Count -gt 0) {
        $jsonObj.mouseSensitivity = [double]$value
    } else {
        $jsonObj | Add-Member -NotePropertyName "mouseSensitivity" -NotePropertyValue ([double]$value) -Force
    }
    return $jsonObj
}

function Write-StandardSettingsFile([string]$path, [object]$jsonObj) {
    if ([string]::IsNullOrWhiteSpace($path)) { return }
    $jsonText = $jsonObj | ConvertTo-Json -Depth 100
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $jsonText + [Environment]::NewLine, $utf8NoBom)
}

function Ensure-MouseNative {
    if ("BoatEyeMouseNative" -as [type]) { return }
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class BoatEyeMouseNative {
    [DllImport("user32.dll", EntryPoint = "SystemParametersInfoW", SetLastError = true)]
    public static extern bool SystemParametersInfoGetInt(uint uiAction, uint uiParam, out int pvParam, uint fWinIni);

    [DllImport("user32.dll", EntryPoint = "SystemParametersInfoW", SetLastError = true)]
    public static extern bool SystemParametersInfoSetUIntPtr(uint uiAction, uint uiParam, UIntPtr pvParam, uint fWinIni);

    [DllImport("user32.dll", EntryPoint = "SystemParametersInfoW", SetLastError = true)]
    public static extern bool SystemParametersInfoIntArray(uint uiAction, uint uiParam, int[] pvParam, uint fWinIni);
}
"@
}

function Show-ConfirmPopup([string]$title, [string]$message) {
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        $result = [System.Windows.Forms.MessageBox]::Show(
            $message,
            $title,
            [System.Windows.Forms.MessageBoxButtons]::YesNo,
            [System.Windows.Forms.MessageBoxIcon]::Question
        )
        return ($result -eq [System.Windows.Forms.DialogResult]::Yes)
    } catch {
        return $true
    }
}

function Show-StatusPopup([string]$title, [string]$message, [bool]$isError) {
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        $icon = if ($isError) { [System.Windows.Forms.MessageBoxIcon]::Error } else { [System.Windows.Forms.MessageBoxIcon]::Information }
        [void][System.Windows.Forms.MessageBox]::Show(
            $message,
            $title,
            [System.Windows.Forms.MessageBoxButtons]::OK,
            $icon
        )
    } catch {
    }
}

function Get-WindowsMouseSettings {
    $mouseReg = Get-ItemProperty -Path "HKCU:\\Control Panel\\Mouse" -ErrorAction Stop

    Ensure-MouseNative
    $SPI_GETMOUSESPEED = 0x0070
    $pointerSpeed = 0
    $ok = [BoatEyeMouseNative]::SystemParametersInfoGetInt($SPI_GETMOUSESPEED, 0, [ref]$pointerSpeed, 0)
    if (-not $ok) {
        $pointerSpeed = [int]$mouseReg.MouseSensitivity
    }

    $mouseSpeed = [int]$mouseReg.MouseSpeed
    $threshold1 = [int]$mouseReg.MouseThreshold1
    $threshold2 = [int]$mouseReg.MouseThreshold2
    $accelOff = ($mouseSpeed -eq 0 -and $threshold1 -eq 0 -and $threshold2 -eq 0)

    return [PSCustomObject]@{
        PointerSpeed = $pointerSpeed
        MouseSpeed = $mouseSpeed
        Threshold1 = $threshold1
        Threshold2 = $threshold2
        AccelDisabled = $accelOff
    }
}

function Set-WindowsMouseSettings([int]$pointerSpeedTarget, [bool]$disableAccel) {
    $pointerSpeedTarget = [int](Clamp-Double $pointerSpeedTarget 1 20)

    Ensure-MouseNative
    $SPI_SETMOUSE = 0x0004
    $SPI_SETMOUSESPEED = 0x0071
    $SPIF_UPDATEINIFILE = 0x01
    $SPIF_SENDCHANGE = 0x02
    $flags = $SPIF_UPDATEINIFILE -bor $SPIF_SENDCHANGE

    # For SPI_SETMOUSESPEED docs require fWinIni = 0 and pvParam as UINT_PTR speed value.
    $okSetSpeed = [BoatEyeMouseNative]::SystemParametersInfoSetUIntPtr($SPI_SETMOUSESPEED, 0, [UIntPtr]::new([uint32]$pointerSpeedTarget), 0)
    if (-not $okSetSpeed) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Failed to set Windows pointer speed via SPI_SETMOUSESPEED (err=$err)."
    }
    Set-ItemProperty -Path "HKCU:\\Control Panel\\Mouse" -Name MouseSensitivity -Value "$pointerSpeedTarget" -Type String

    if ($disableAccel) {
        Set-WindowsMouseAccelParams -mouseSpeed 0 -threshold1 0 -threshold2 0
    }
}

function Set-WindowsMouseAccelParams([int]$mouseSpeed, [int]$threshold1, [int]$threshold2) {
    Ensure-MouseNative
    $SPI_SETMOUSE = 0x0004
    $SPIF_UPDATEINIFILE = 0x01
    $SPIF_SENDCHANGE = 0x02
    $flags = $SPIF_UPDATEINIFILE -bor $SPIF_SENDCHANGE

    $mouseSpeed = [int](Clamp-Double $mouseSpeed 0 2)
    $threshold1 = [int](Clamp-Double $threshold1 0 20)
    $threshold2 = [int](Clamp-Double $threshold2 0 20)

    Set-ItemProperty -Path "HKCU:\\Control Panel\\Mouse" -Name MouseSpeed -Value "$mouseSpeed" -Type String
    Set-ItemProperty -Path "HKCU:\\Control Panel\\Mouse" -Name MouseThreshold1 -Value "$threshold1" -Type String
    Set-ItemProperty -Path "HKCU:\\Control Panel\\Mouse" -Name MouseThreshold2 -Value "$threshold2" -Type String

    $mouseParams = [int[]]($threshold1, $threshold2, $mouseSpeed)
    $okSetMouse = [BoatEyeMouseNative]::SystemParametersInfoIntArray($SPI_SETMOUSE, 0, $mouseParams, $flags)
    if (-not $okSetMouse) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Failed to set Windows mouse accel params via SPI_SETMOUSE (err=$err)."
    }
}

function Resolve-BackupRestorePath([string]$requestedPath, [bool]$restoreLatest) {
    if (-not [string]::IsNullOrWhiteSpace($requestedPath)) {
        if (-not (Test-Path -LiteralPath $requestedPath -PathType Leaf)) {
            throw "Restore backup file not found: $requestedPath"
        }
        return (Get-Item -LiteralPath $requestedPath -ErrorAction Stop).FullName
    }

    if (-not $restoreLatest) {
        throw "No restore backup path provided. Use -RestoreLatestBackup or -RestoreBackupPath."
    }

    $backupDir = Join-Path $env:LOCALAPPDATA "Toolscreen\\backups"
    if (-not (Test-Path -LiteralPath $backupDir -PathType Container)) {
        throw "Backup directory not found: $backupDir"
    }

    $latest = Get-ChildItem -LiteralPath $backupDir -Filter "boat_eye_mouse_backup_*.json" -File -ErrorAction Stop |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($null -eq $latest) { throw "No boat calibration backups found in $backupDir." }
    return $latest.FullName
}

function Get-InstalledSoftwareNames {
    $paths = @(
        "HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\*",
        "HKLM:\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\*",
        "HKCU:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\*"
    )

    $names = New-Object 'System.Collections.Generic.List[string]'
    foreach ($p in $paths) {
        try {
            Get-ItemProperty -Path $p -ErrorAction SilentlyContinue | ForEach-Object {
                if ($_.DisplayName) {
                    $name = [string]$_.DisplayName
                    if (-not [string]::IsNullOrWhiteSpace($name)) {
                        $names.Add($name) | Out-Null
                    }
                }
            }
        } catch {
        }
    }

    return $names
}

function Get-MouseVendorLabel([string]$name, [string]$instanceId) {
    $n = if ($name) { $name.ToLowerInvariant() } else { "" }
    $id = if ($instanceId) { $instanceId.ToUpperInvariant() } else { "" }

    if ($id -match "VID_046D") { return "Logitech" }
    if ($id -match "VID_1532") { return "Razer" }
    if ($id -match "VID_1038") { return "SteelSeries" }
    if ($id -match "VID_1B1C") { return "Corsair" }
    if ($id -match "VID_258A") { return "Glorious" }
    if ($id -match "VID_045E") { return "Microsoft" }
    if ($id -match "VID_0B05") { return "ASUS" }

    if ($n -match "logitech|logi") { return "Logitech" }
    if ($n -match "razer") { return "Razer" }
    if ($n -match "steelseries") { return "SteelSeries" }
    if ($n -match "corsair") { return "Corsair" }
    if ($n -match "glorious") { return "Glorious" }
    if ($n -match "microsoft") { return "Microsoft" }
    if ($n -match "asus|rog") { return "ASUS" }

    return "Unknown"
}

function Detect-MouseSetupContext {
    $vendorProfiles = @{
        "Logitech" = @{ Software = "Logitech G HUB"; Tokens = @("Logitech G HUB", "Logitech Gaming Software", "Logi Options", "Logi Options+") }
        "Razer" = @{ Software = "Razer Synapse"; Tokens = @("Razer Synapse", "Razer Central") }
        "SteelSeries" = @{ Software = "SteelSeries GG"; Tokens = @("SteelSeries GG", "SteelSeries Engine") }
        "Corsair" = @{ Software = "Corsair iCUE"; Tokens = @("Corsair iCUE", "Corsair Utility Engine") }
        "Glorious" = @{ Software = "Glorious CORE"; Tokens = @("Glorious CORE") }
        "ASUS" = @{ Software = "Armoury Crate"; Tokens = @("Armoury Crate") }
        "Microsoft" = @{ Software = "Microsoft Mouse and Keyboard Center"; Tokens = @("Mouse and Keyboard Center") }
    }

    $installedNames = Get-InstalledSoftwareNames

    $devices = @()

    try {
        if (Get-Command Get-PnpDevice -ErrorAction SilentlyContinue) {
            Get-PnpDevice -Class Mouse -PresentOnly -ErrorAction SilentlyContinue | ForEach-Object {
                $name = [string]$_.FriendlyName
                if ([string]::IsNullOrWhiteSpace($name)) { $name = [string]$_.Name }
                $instanceId = [string]$_.InstanceId
                if ([string]::IsNullOrWhiteSpace($name)) { return }
                $devices += [PSCustomObject]@{ Name = $name; InstanceId = $instanceId }
            }
        }
    } catch {
    }

    if ($devices.Count -eq 0) {
        try {
            Get-CimInstance Win32_PointingDevice -ErrorAction SilentlyContinue | ForEach-Object {
                $name = [string]$_.Name
                $instanceId = [string]$_.PNPDeviceID
                if ([string]::IsNullOrWhiteSpace($name)) { return }
                $devices += [PSCustomObject]@{ Name = $name; InstanceId = $instanceId }
            }
        } catch {
        }
    }

    $hintList = @()
    $adviceList = @()
    $seenVendors = @{}

    if ($devices.Count -eq 0) {
        $adviceList += "No mouse device details detected via Windows APIs; enter DPI manually in Boat setup."
    }

    foreach ($device in $devices) {
        $vendor = Get-MouseVendorLabel -name ([string]$device.Name) -instanceId ([string]$device.InstanceId)
        if ($vendor -eq "Unknown") { continue }
        if ($seenVendors.ContainsKey($vendor)) { continue }
        $seenVendors[$vendor] = $true

        $profile = $vendorProfiles[$vendor]
        $softwareName = if ($null -ne $profile) { [string]$profile.Software } else { "Manufacturer software" }
        $tokens = if ($null -ne $profile) { [string[]]$profile.Tokens } else { @() }

        $installed = $false
        foreach ($installedName in $installedNames) {
            foreach ($token in $tokens) {
                if ($installedName.ToLowerInvariant().Contains($token.ToLowerInvariant())) {
                    $installed = $true
                    break
                }
            }
            if ($installed) { break }
        }

        $hintList += [PSCustomObject]@{
            Vendor = $vendor
            Software = $softwareName
            Installed = $installed
            Advice = if ($installed) {
                "Use $softwareName to set DPI."
            } else {
                "Install $softwareName (or your mouse vendor utility) to change DPI precisely."
            }
        }

        if (-not $installed) {
            $adviceList += "$vendor detected, but $softwareName was not found. Install vendor software or use onboard DPI button."
        }
    }

    if ($hintList.Count -eq 0) {
        $adviceList += "Vendor software was not auto-detected. If your mouse has no native DPI controls, install your vendor utility."
    }

    $normalizedDevices = @()
    foreach ($device in $devices) {
        $vendor = Get-MouseVendorLabel -name ([string]$device.Name) -instanceId ([string]$device.InstanceId)
        $normalizedDevices += [PSCustomObject]@{
            Name = [string]$device.Name
            InstanceId = [string]$device.InstanceId
            Vendor = $vendor
        }
    }

    return [PSCustomObject]@{
        Devices = $normalizedDevices
        SoftwareHints = @($hintList)
        Advice = @($adviceList)
    }
}

$resultPayload = $null

try {
    $disableMouseAccel = -not $NoDisableMouseAccel.IsPresent
    $enableRawInput = -not $NoEnableRawInput.IsPresent
    $lowestSkipChoiceOne = Convert-ToBool -value $LowestSkipChoiceOne -defaultValue $true -name "LowestSkipChoiceOne"
    $includeCursorInRanking = Convert-ToBool -value $IncludeCursorInRanking -defaultValue $true -name "IncludeCursorInRanking"
    $preferHigherDpi = Convert-ToBool -value $PreferHigherDpi -defaultValue $false -name "PreferHigherDpi"

    if ($TargetDpi -le 0) { throw "TargetDpi must be > 0." }
    if ($TargetWindowsSpeed -lt 0 -or $TargetWindowsSpeed -gt 20) { throw "TargetWindowsSpeed must be between 0 and 20." }
    if ($ManualCurrentWindowsSpeed -lt 0 -or $ManualCurrentWindowsSpeed -gt 20) {
        throw "ManualCurrentWindowsSpeed must be between 0 and 20."
    }
    if ($MaxRecommendedPixelSkipping -le 0.0) {
        throw "MaxRecommendedPixelSkipping must be > 0."
    }

    $resolvedOptionsPath = Resolve-OptionsPath $OptionsPath
    $parsed = Parse-OptionsFile $resolvedOptionsPath
    $optionsMap = $parsed.Map

    $resolvedStandardSettingsPath = Resolve-StandardSettingsPath -requestedPath $StandardSettingsPath -resolvedOptionsPath $resolvedOptionsPath
    $standardSettingsParsed = $null
    $standardSettingsWarning = ""
    $standardSettingsSensitivity = $null
    $hasStandardSettingsSensitivity = $false
    if (-not [string]::IsNullOrWhiteSpace($resolvedStandardSettingsPath)) {
        try {
            $standardSettingsParsed = Parse-StandardSettingsFile -path $resolvedStandardSettingsPath
            if ($null -ne $standardSettingsParsed -and $standardSettingsParsed.HasSensitivity) {
                $standardSettingsSensitivity = Clamp-Double ([double]$standardSettingsParsed.Sensitivity) 0.0 1.0
                $hasStandardSettingsSensitivity = $true
            }
        } catch {
            $standardSettingsWarning = "Failed to parse standardsettings.json: " + $_.Exception.Message
        }
    }

    $optionsSensitivity = $null
    $hasOptionsSensitivity = $false
    if ($optionsMap.ContainsKey("mouseSensitivity")) {
        [double]$parsedSens = 0.5
        if ([double]::TryParse($optionsMap["mouseSensitivity"], [System.Globalization.NumberStyles]::Float,
                [System.Globalization.CultureInfo]::InvariantCulture, [ref]$parsedSens)) {
            $optionsSensitivity = Clamp-Double $parsedSens 0.0 1.0
            $hasOptionsSensitivity = $true
        }
    }

    $currentSensitivity = if ($hasOptionsSensitivity) { [double]$optionsSensitivity } else { 0.5 }
    $currentSensitivitySource = if ($hasOptionsSensitivity) { "options.txt" } else { "fallback-default" }
    if ($InputMode -eq "auto") {
        if ($hasStandardSettingsSensitivity) {
            $currentSensitivity = [double]$standardSettingsSensitivity
            $currentSensitivitySource = "standardsettings.json"
        }
    } else {
        if ($PreferredStandardSensitivity -lt 0.0) {
            throw "InputMode=manual requires PreferredStandardSensitivity."
        }
        $currentSensitivity = Clamp-Double $PreferredStandardSensitivity 0.0 1.0
        $currentSensitivitySource = "manual-input"
    }

    $rawInputCurrent = ""
    if ($optionsMap.ContainsKey("rawMouseInput")) {
        $rawInputCurrent = "$($optionsMap["rawMouseInput"])"
    } else {
        $rawInputCurrent = "(missing)"
    }

    $screenEffectScaleCurrent = $null
    if ($optionsMap.ContainsKey("screenEffectScale")) {
        [double]$parsedScreen = 0.0
        if ([double]::TryParse($optionsMap["screenEffectScale"], [System.Globalization.NumberStyles]::Float,
                [System.Globalization.CultureInfo]::InvariantCulture, [ref]$parsedScreen)) {
            $screenEffectScaleCurrent = Clamp-Double $parsedScreen 0.0 1.0
        }
    }
    $fovEffectScaleCurrent = $null
    if ($optionsMap.ContainsKey("fovEffectScale")) {
        [double]$parsedFov = 0.0
        if ([double]::TryParse($optionsMap["fovEffectScale"], [System.Globalization.NumberStyles]::Float,
                [System.Globalization.CultureInfo]::InvariantCulture, [ref]$parsedFov)) {
            $fovEffectScaleCurrent = Clamp-Double $parsedFov 0.0 1.0
        }
    }

    if ($ApplyVisualEffectsOnly.IsPresent) {
        $visualScreenScale = Clamp-Double $TargetScreenEffectScale 0.0 1.0
        $visualFovScale = Clamp-Double $TargetFovEffectScale 0.0 1.0

        $resultPayload = [PSCustomObject]@{
            ok = $true
            mode = "visual-effects-only"
            optionsPath = $resolvedOptionsPath
            standardSettingsPath = $resolvedStandardSettingsPath
            current = [PSCustomObject]@{
                screenEffectScale = if ($null -ne $screenEffectScaleCurrent) { [Math]::Round([double]$screenEffectScaleCurrent, 8) } else { $null }
                fovEffectScale = if ($null -ne $fovEffectScaleCurrent) { [Math]::Round([double]$fovEffectScaleCurrent, 8) } else { $null }
            }
            apply = [PSCustomObject]@{
                requested = $Apply.IsPresent
                applied = $false
                canceled = $false
                message = ""
                targetScreenEffectScale = [Math]::Round([double]$visualScreenScale, 8)
                targetFovEffectScale = [Math]::Round([double]$visualFovScale, 8)
                after = $null
            }
        }
        if (-not [string]::IsNullOrWhiteSpace($standardSettingsWarning)) {
            $resultPayload | Add-Member -NotePropertyName "warning" -NotePropertyValue $standardSettingsWarning -Force
        }

        Write-Info ""
        Write-Info "=== Visual Effects Apply Report ==="
        Write-Info "options.txt: $resolvedOptionsPath"
        if (-not [string]::IsNullOrWhiteSpace($resolvedStandardSettingsPath)) {
            Write-Info "standardsettings.json: $resolvedStandardSettingsPath"
        } else {
            Write-Info "standardsettings.json: (not found)"
        }
        Write-Info "Current screenEffectScale: $(if ($null -ne $screenEffectScaleCurrent) { $screenEffectScaleCurrent } else { "(missing)" })"
        Write-Info "Current fovEffectScale:    $(if ($null -ne $fovEffectScaleCurrent) { $fovEffectScaleCurrent } else { "(missing)" })"
        Write-Info "Target screenEffectScale:  $visualScreenScale"
        Write-Info "Target fovEffectScale:     $visualFovScale"

        if (-not $Apply.IsPresent) {
            Write-Info ""
            Write-Info "Dry run only. Add -Apply to write settings."
            if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 0 }
            exit 0
        }

        if (-not $NoPopup.IsPresent) {
            $ok = Show-ConfirmPopup -title "Apply Visual Effects" -message @"
Apply visual effects settings?

options.txt:
$resolvedOptionsPath

screenEffectScale -> $visualScreenScale
fovEffectScale    -> $visualFovScale
"@
            if (-not $ok) {
                $resultPayload.apply.canceled = $true
                $resultPayload.apply.message = "Apply canceled."
                if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 0 }
                exit 0
            }
        }

        try {
            Set-OptionValue $parsed.Lines "screenEffectScale" (Format-DoubleInvariant $visualScreenScale)
            Set-OptionValue $parsed.Lines "fovEffectScale" (Format-DoubleInvariant $visualFovScale)
            Write-OptionsFile $resolvedOptionsPath $parsed.Lines

            $standardSettingsUpdated = $false
            if (-not [string]::IsNullOrWhiteSpace($resolvedStandardSettingsPath)) {
                try {
                    if ($null -eq $standardSettingsParsed) {
                        $standardSettingsParsed = Parse-StandardSettingsFile -path $resolvedStandardSettingsPath
                    }
                    if ($null -eq $standardSettingsParsed -or $null -eq $standardSettingsParsed.Json) {
                        throw "standardsettings content was empty."
                    }
                    $updatedStandardSettings = $standardSettingsParsed.Json
                    $updatedStandardSettings | Add-Member -NotePropertyName "screenEffectScale" -NotePropertyValue ([double]$visualScreenScale) -Force
                    $updatedStandardSettings | Add-Member -NotePropertyName "fovEffectScale" -NotePropertyValue ([double]$visualFovScale) -Force
                    Write-StandardSettingsFile -path $resolvedStandardSettingsPath -jsonObj $updatedStandardSettings
                    $standardSettingsUpdated = $true
                } catch {
                    Write-Info "Warning: failed to update standardsettings.json: $($_.Exception.Message)"
                }
            }

            $resultPayload.apply.applied = $true
            $resultPayload.apply.message = "Visual effects applied."
            $resultPayload.apply.after = [PSCustomObject]@{
                screenEffectScale = $visualScreenScale
                fovEffectScale = $visualFovScale
                standardSettingsUpdated = $standardSettingsUpdated
                standardSettingsPath = if ([string]::IsNullOrWhiteSpace($resolvedStandardSettingsPath)) { $null } else { $resolvedStandardSettingsPath }
            }
            Write-Info "Visual effects applied."
            if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 0 }
            exit 0
        } catch {
            $msg = $_.Exception.Message
            $resultPayload.ok = $false
            $resultPayload.apply.message = "Apply failed: $msg"
            if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 1 }
            throw
        }
    }

    if ($RestoreLatestBackup.IsPresent -or -not [string]::IsNullOrWhiteSpace($RestoreBackupPath)) {
        $resolvedRestorePath = Resolve-BackupRestorePath -requestedPath $RestoreBackupPath -restoreLatest:$RestoreLatestBackup.IsPresent
        $backupRaw = Get-Content -LiteralPath $resolvedRestorePath -Raw -Encoding UTF8
        $backupJson = $backupRaw | ConvertFrom-Json
        if ($null -eq $backupJson -or $null -eq $backupJson.before) {
            throw "Restore backup JSON is missing 'before' block: $resolvedRestorePath"
        }
        $before = $backupJson.before

        $restoreSensitivity = $null
        if ($null -ne $before.standardSettingsSensitivity) {
            $restoreSensitivity = Clamp-Double ([double]$before.standardSettingsSensitivity) 0.0 1.0
        } elseif ($null -ne $before.optionsSensitivity) {
            $restoreSensitivity = Clamp-Double ([double]$before.optionsSensitivity) 0.0 1.0
        } elseif ($null -ne $before.mouseSensitivity) {
            $restoreSensitivity = Clamp-Double ([double]$before.mouseSensitivity) 0.0 1.0
        }

        $restoreRawInput = $null
        if ($null -ne $before.rawMouseInput) {
            $rawText = "$($before.rawMouseInput)".Trim().ToLowerInvariant()
            if ($rawText -eq "true" -or $rawText -eq "false") { $restoreRawInput = $rawText }
        }

        $restoreScreen = $null
        if ($null -ne $before.screenEffectScale) { $restoreScreen = Clamp-Double ([double]$before.screenEffectScale) 0.0 1.0 }
        $restoreFov = $null
        if ($null -ne $before.fovEffectScale) { $restoreFov = Clamp-Double ([double]$before.fovEffectScale) 0.0 1.0 }

        $restorePointerSpeed = 10
        if ($null -ne $before.windowsPointerSpeed) {
            $restorePointerSpeed = [int](Clamp-Double ([double]$before.windowsPointerSpeed) 1 20)
        }
        $restoreMouseSpeed = 0
        if ($null -ne $before.windowsMouseSpeed) {
            $restoreMouseSpeed = [int](Clamp-Double ([double]$before.windowsMouseSpeed) 0 2)
        }
        $restoreThreshold1 = 0
        if ($null -ne $before.windowsThreshold1) {
            $restoreThreshold1 = [int](Clamp-Double ([double]$before.windowsThreshold1) 0 20)
        }
        $restoreThreshold2 = 0
        if ($null -ne $before.windowsThreshold2) {
            $restoreThreshold2 = [int](Clamp-Double ([double]$before.windowsThreshold2) 0 20)
        }
        $restoreNbbRegistrySensitivity = $null
        if ($null -ne $before.nbbRegistrySensitivity) {
            $restoreNbbRegistrySensitivity = Clamp-Double ([double]$before.nbbRegistrySensitivity) 0.0 1.0
        }

        $resultPayload = [PSCustomObject]@{
            ok = $true
            mode = "restore-from-backup"
            optionsPath = $resolvedOptionsPath
            standardSettingsPath = $resolvedStandardSettingsPath
            restore = [PSCustomObject]@{
                backupPath = $resolvedRestorePath
            }
            apply = [PSCustomObject]@{
                requested = $Apply.IsPresent
                applied = $false
                canceled = $false
                message = ""
                after = $null
            }
        }

        if (-not $Apply.IsPresent) {
            Write-Info "Restore dry run. Add -Apply to restore backup values."
            if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 0 }
            exit 0
        }

        if (-not $NoPopup.IsPresent) {
            $ok = Show-ConfirmPopup -title "Restore Boat Calibration" -message @"
Restore previous values from backup?

$resolvedRestorePath
"@
            if (-not $ok) {
                $resultPayload.apply.canceled = $true
                $resultPayload.apply.message = "Restore canceled."
                if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 0 }
                exit 0
            }
        }

        try {
            if ($null -ne $restoreSensitivity) {
                Set-OptionValue $parsed.Lines "mouseSensitivity" (Format-DoubleInvariant $restoreSensitivity)
            }
            if ($null -ne $restoreRawInput) {
                Set-OptionValue $parsed.Lines "rawMouseInput" $restoreRawInput
            }
            if ($null -ne $restoreScreen) {
                Set-OptionValue $parsed.Lines "screenEffectScale" (Format-DoubleInvariant $restoreScreen)
            }
            if ($null -ne $restoreFov) {
                Set-OptionValue $parsed.Lines "fovEffectScale" (Format-DoubleInvariant $restoreFov)
            }
            Write-OptionsFile $resolvedOptionsPath $parsed.Lines

            $standardSettingsUpdated = $false
            if (-not [string]::IsNullOrWhiteSpace($resolvedStandardSettingsPath)) {
                try {
                    if ($null -eq $standardSettingsParsed) {
                        $standardSettingsParsed = Parse-StandardSettingsFile -path $resolvedStandardSettingsPath
                    }
                    if ($null -eq $standardSettingsParsed -or $null -eq $standardSettingsParsed.Json) {
                        throw "standardsettings content was empty."
                    }
                    $updatedStandardSettings = $standardSettingsParsed.Json
                    if ($null -ne $restoreSensitivity) {
                        $updatedStandardSettings = Set-StandardSettingsSensitivity -jsonObj $updatedStandardSettings -value $restoreSensitivity
                    }
                    if ($null -ne $restoreScreen) {
                        $updatedStandardSettings | Add-Member -NotePropertyName "screenEffectScale" -NotePropertyValue ([double]$restoreScreen) -Force
                    }
                    if ($null -ne $restoreFov) {
                        $updatedStandardSettings | Add-Member -NotePropertyName "fovEffectScale" -NotePropertyValue ([double]$restoreFov) -Force
                    }
                    Write-StandardSettingsFile -path $resolvedStandardSettingsPath -jsonObj $updatedStandardSettings
                    $standardSettingsUpdated = $true
                } catch {
                    Write-Info "Warning: failed to update standardsettings.json: $($_.Exception.Message)"
                }
            }

            Set-WindowsMouseSettings -pointerSpeedTarget $restorePointerSpeed -disableAccel:$false
            Set-WindowsMouseAccelParams -mouseSpeed $restoreMouseSpeed -threshold1 $restoreThreshold1 -threshold2 $restoreThreshold2
            if ($null -ne $restoreNbbRegistrySensitivity) {
                Set-NbbRegistryDouble -name "sensitivity" -value $restoreNbbRegistrySensitivity
            }
            $windowsAfter = Get-WindowsMouseSettings

            $resultPayload.apply.applied = $true
            $resultPayload.apply.message = "Backup restored."
            $resultPayload.apply.after = [PSCustomObject]@{
                windowsPointerSpeed = $windowsAfter.PointerSpeed
                windowsMouseSpeed = $windowsAfter.MouseSpeed
                windowsThreshold1 = $windowsAfter.Threshold1
                windowsThreshold2 = $windowsAfter.Threshold2
                windowsAccelDisabled = $windowsAfter.AccelDisabled
                minecraftSensitivity = $restoreSensitivity
                rawMouseInput = $restoreRawInput
                screenEffectScale = $restoreScreen
                fovEffectScale = $restoreFov
                standardSettingsUpdated = $standardSettingsUpdated
                nbbRegistrySensitivity = Get-NbbRegistryDouble -name "sensitivity"
            }
            if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 0 }
            exit 0
        } catch {
            $msg = $_.Exception.Message
            $resultPayload.ok = $false
            $resultPayload.apply.message = "Restore failed: $msg"
            if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 1 }
            throw
        }
    }

    if ($CurrentDpi -le 0) {
        if ($script:EmitJsonOnly) {
            throw "CurrentDpi is required when using -Json."
        }
        $dpiInput = Read-Host "Enter your CURRENT mouse DPI (required, e.g. 800)"
        [int]$parsedDpi = 0
        if (-not [int]::TryParse($dpiInput, [ref]$parsedDpi) -or $parsedDpi -le 0) {
            throw "Invalid DPI value '$dpiInput'."
        }
        $CurrentDpi = $parsedDpi
    }

    $windowsBefore = Get-WindowsMouseSettings
    $sensListResult = Get-BoatEyeSensitivities -localPath $SensitivityListPath -url $SensitivityListUrl
    $sensList = [double[]]$sensListResult.Values
    $mouseContext = Detect-MouseSetupContext

    $currentCursorSpeedForCalc = [int](Clamp-Double $windowsBefore.PointerSpeed 1 20)
    $currentCursorSource = "windows-live"
    if ($InputMode -eq "manual") {
        if ($ManualCurrentWindowsSpeed -gt 0) {
            $currentCursorSpeedForCalc = [int](Clamp-Double $ManualCurrentWindowsSpeed 1 20)
            $currentCursorSource = "manual-input"
        } else {
            $currentCursorSource = "windows-live-fallback"
        }
    }

    $effectiveTargetWindowsSpeed = $TargetWindowsSpeed
    if ($effectiveTargetWindowsSpeed -eq 0) {
        $effectiveTargetWindowsSpeed = $currentCursorSpeedForCalc
    }

    $targetRawSensitivity = 0.0
    $targetMappedSource = "target-mapped"
    if ($TargetSensitivity -ge 0.0) {
        $targetRawSensitivity = Clamp-Double $TargetSensitivity 0.0 1.0
        $targetMappedSource = "target-mapped-fixed"
    } else {
        $targetRawSensitivity = Compute-RawSensitivityForTarget `
            -currentSensitivity $currentSensitivity `
            -currentDpi $CurrentDpi `
            -currentCursorSpeed $currentCursorSpeedForCalc `
            -targetDpi $TargetDpi `
            -targetCursorSpeed $effectiveTargetWindowsSpeed
    }

    $targetMappedRecommendation = Build-RecommendationFromRawSensitivity -rawSensitivity $targetRawSensitivity -targetDpi $TargetDpi `
        -targetCursorSpeed $effectiveTargetWindowsSpeed -sourceMode $targetMappedSource -sensList $sensList

    $pixelPerfectAutoRecommendation = Compute-PixelPerfectAutoRecommendation `
        -currentDpi $CurrentDpi `
        -currentSensitivity $currentSensitivity `
        -currentCursorSpeed $currentCursorSpeedForCalc `
        -sensList $sensList `
        -preferLowestPixelSkipping:$PreferLowestPixelSkipping `
        -targetCursorConstraint $TargetWindowsSpeed `
        -recommendationChoice $RecommendationChoice `
        -lowestSkipChoiceOne:$lowestSkipChoiceOne `
        -includeCursorInRanking:$includeCursorInRanking `
        -preferHigherDpi:$preferHigherDpi `
        -maxRecommendedPixelSkipping $MaxRecommendedPixelSkipping

    $pixelPerfectPreferredRecommendation = $null
    if ($TargetWindowsSpeed -gt 0) {
        $pixelPerfectPreferredRecommendation = Compute-PixelPerfectRecommendationForCursorSpeed `
            -currentDpi $CurrentDpi `
            -currentSensitivity $currentSensitivity `
            -currentCursorSpeed $currentCursorSpeedForCalc `
            -targetCursorSpeed $TargetWindowsSpeed `
            -sensList $sensList
    }

    $activeRecommendation = $targetMappedRecommendation
    if ($RecommendationMode -eq "pixel-perfect") {
        # Enforced policy: always use lowest-skipping pixel-perfect recommendation.
        $activeRecommendation = $pixelPerfectAutoRecommendation

        if ($null -ne $pixelPerfectPreferredRecommendation) {
            $preferredSkip = [double]$pixelPerfectPreferredRecommendation.EstimatedPixelSkipping
            $autoSkip = [double]$pixelPerfectAutoRecommendation.EstimatedPixelSkipping
            $preferredOverridden = $false
            if ($TargetWindowsSpeed -gt 0) {
                $preferredOverridden = ([int]$activeRecommendation.TargetCursorSpeed -ne [int]$TargetWindowsSpeed)
            }
            $activeRecommendation | Add-Member -NotePropertyName PreferredSpeedOverridden -NotePropertyValue $preferredOverridden -Force
            $activeRecommendation | Add-Member -NotePropertyName PreferredSpeedSkipping `
                -NotePropertyValue ([Math]::Round($preferredSkip, 2)) -Force
            $activeRecommendation | Add-Member -NotePropertyName AutoPixelSkipping `
                -NotePropertyValue ([Math]::Round($autoSkip, 2)) -Force
        }
    }

    $recommendedSensitivity = [double]$activeRecommendation.SelectedSensitivity
    $recommendedSensitivityText = Format-DoubleInvariant $recommendedSensitivity
    $recommendedSensitivityAlt = $activeRecommendation.SecondarySensitivity
    $recommendedSensitivityAltText = if ($null -ne $recommendedSensitivityAlt) {
        Format-DoubleInvariant ([double]$recommendedSensitivityAlt)
    } else {
        $null
    }

    $rawInputTargetLabel = if ($enableRawInput) { "true" } else { "unchanged" }

    $resultPayload = [PSCustomObject]@{
        ok = $true
        optionsPath = $resolvedOptionsPath
        standardSettingsPath = $resolvedStandardSettingsPath
        recommendationMode = $RecommendationMode
        inputMode = $InputMode
        sensitivityListSource = $sensListResult.Source
        sensitivityListSourceType = $sensListResult.SourceType
        sensitivityListParseStats = $sensListResult.ParseStats
        current = [PSCustomObject]@{
            inputMode = $InputMode
            minecraftSensitivity = [Math]::Round([double]$currentSensitivity, 8)
            sensitivitySource = $currentSensitivitySource
            optionsSensitivity = if ($hasOptionsSensitivity) { [Math]::Round([double]$optionsSensitivity, 8) } else { $null }
            standardSettingsSensitivity = if ($hasStandardSettingsSensitivity) { [Math]::Round([double]$standardSettingsSensitivity, 8) } else { $null }
            standardSettingsDetected = $hasStandardSettingsSensitivity
            preferredStandardSensitivity = if ($PreferredStandardSensitivity -ge 0.0) {
                [Math]::Round((Clamp-Double $PreferredStandardSensitivity 0.0 1.0), 8)
            } else {
                $null
            }
            screenEffectScale = if ($null -ne $screenEffectScaleCurrent) { [Math]::Round([double]$screenEffectScaleCurrent, 8) } else { $null }
            fovEffectScale = if ($null -ne $fovEffectScaleCurrent) { [Math]::Round([double]$fovEffectScaleCurrent, 8) } else { $null }
            rawMouseInput = $rawInputCurrent
            dpi = $CurrentDpi
            currentCursorSpeedForCalc = $currentCursorSpeedForCalc
            currentCursorSource = $currentCursorSource
            manualCurrentWindowsSpeed = if ($ManualCurrentWindowsSpeed -gt 0) { [int](Clamp-Double $ManualCurrentWindowsSpeed 1 20) } else { $null }
            windowsPointerSpeed = $windowsBefore.PointerSpeed
            windowsMouseSpeed = $windowsBefore.MouseSpeed
            windowsThreshold1 = $windowsBefore.Threshold1
            windowsThreshold2 = $windowsBefore.Threshold2
            windowsAccelDisabled = $windowsBefore.AccelDisabled
            nbbRegistrySensitivity = Get-NbbRegistryDouble -name "sensitivity"
        }
        recommendations = [PSCustomObject]@{
            active = $activeRecommendation
            pixelPerfectAuto = $pixelPerfectAutoRecommendation
            pixelPerfectPreferred = $pixelPerfectPreferredRecommendation
            targetMapped = $targetMappedRecommendation
        }
        mouse = $mouseContext
        apply = [PSCustomObject]@{
            requested = $Apply.IsPresent
            applied = $false
            canceled = $false
            backupPath = ""
            message = ""
            visualEffectsRequested = $ApplyVisualEffects.IsPresent
            targetScreenEffectScale = [Math]::Round((Clamp-Double $TargetScreenEffectScale 0.0 1.0), 8)
            targetFovEffectScale = [Math]::Round((Clamp-Double $TargetFovEffectScale 0.0 1.0), 8)
            after = $null
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($standardSettingsWarning)) {
        $resultPayload | Add-Member -NotePropertyName "warning" -NotePropertyValue $standardSettingsWarning -Force
    }

    Write-Info ""
    Write-Info "=== Boat Eye Mouse Calibration Report ==="
    Write-Info "options.txt: $resolvedOptionsPath"
    if (-not [string]::IsNullOrWhiteSpace($resolvedStandardSettingsPath)) {
        Write-Info "standardsettings.json: $resolvedStandardSettingsPath"
    } else {
        Write-Info "standardsettings.json: (not found)"
    }
    Write-Info "sensitivity list source: $($sensListResult.SourceType) -> $($sensListResult.Source)"
    if ($null -ne $sensListResult.ParseStats) {
        Write-Info "sensitivity list parse: lines=$($sensListResult.ParseStats.lineCount), parsed=$($sensListResult.ParseStats.parsedFirstColumnCount), accepted=$($sensListResult.ParseStats.acceptedRangeCount), unique=$($sensListResult.ParseStats.uniqueCount)"
    }
    if (-not [string]::IsNullOrWhiteSpace($standardSettingsWarning)) {
        Write-Info "standardsettings warning: $standardSettingsWarning"
    }
    Write-Info ""
    Write-Info "Current:"
    Write-Info "  Input mode:                 $InputMode"
    Write-Info "  Sensitivity source:         $currentSensitivitySource"
    Write-Info "  Minecraft mouseSensitivity: $currentSensitivity"
    if ($hasOptionsSensitivity) {
        Write-Info "  options.txt sensitivity:    $optionsSensitivity"
    } else {
        Write-Info "  options.txt sensitivity:    (missing)"
    }
    if ($hasStandardSettingsSensitivity) {
        Write-Info "  standardsettings sensitivity: $standardSettingsSensitivity"
    } elseif (-not [string]::IsNullOrWhiteSpace($resolvedStandardSettingsPath)) {
        Write-Info "  standardsettings sensitivity: (missing)"
    }
    Write-Info "  Minecraft rawMouseInput:    $rawInputCurrent"
    if ($null -ne $screenEffectScaleCurrent) { Write-Info "  screenEffectScale:          $screenEffectScaleCurrent" }
    if ($null -ne $fovEffectScaleCurrent) { Write-Info "  fovEffectScale:             $fovEffectScaleCurrent" }
    Write-Info "  Mouse DPI:                  $CurrentDpi"
    Write-Info "  Windows pointer speed:      $($windowsBefore.PointerSpeed)"
    Write-Info "  Cursor used for calc:       $currentCursorSpeedForCalc ($currentCursorSource)"
    Write-Info "  Windows accel disabled:     $($windowsBefore.AccelDisabled)"
    Write-Info ""
    Write-Info "Active Recommendation ($($activeRecommendation.Source)):"
    Write-Info "  Suggested DPI:              $($activeRecommendation.TargetDpiRounded)"
    Write-Info "  Suggested cursor speed:     $($activeRecommendation.TargetCursorSpeed)"
    Write-Info "  Raw sensitivity:            $($activeRecommendation.RawSensitivity)"
    if ($null -ne $activeRecommendation.SecondarySensitivity) {
        Write-Info "  Closest valid sensitivities: $($activeRecommendation.PrimarySensitivity) or $($activeRecommendation.SecondarySensitivity)"
    } else {
        Write-Info "  Closest valid sensitivity:   $($activeRecommendation.PrimarySensitivity)"
    }
    Write-Info "  Selected sensitivity:        $($activeRecommendation.SelectedSensitivity)"
    Write-Info "  Estimated pixel skipping:    $($activeRecommendation.EstimatedPixelSkipping)"
    Write-Info ""
    Write-Info "Apply Targets:"
    Write-Info "  Disable mouse accel:        $disableMouseAccel"
    Write-Info "  Enable raw input:           $enableRawInput"
    Write-Info "  Include cursor in ranking:  $includeCursorInRanking"
    Write-Info "  Prefer higher DPI:          $preferHigherDpi"
    Write-Info "  Max skip filter:            $MaxRecommendedPixelSkipping"
    if ($ApplyVisualEffects.IsPresent) {
        Write-Info ("  Apply visual effects:       true (screenEffectScale=" + (Clamp-Double $TargetScreenEffectScale 0.0 1.0) + ", fovEffectScale=" + (Clamp-Double $TargetFovEffectScale 0.0 1.0) + ")")
    } else {
        Write-Info "  Apply visual effects:       false"
    }
    if ($activeRecommendation.TargetDpiRounded -ne $CurrentDpi) {
        Write-Info "  DPI apply status:           manual (set in your mouse software: $CurrentDpi -> $($activeRecommendation.TargetDpiRounded))"
    } else {
        Write-Info "  DPI apply status:           unchanged ($CurrentDpi)"
    }
    Write-Info ""
    Write-Info "Mouse software hints:"
    if ($mouseContext.SoftwareHints.Count -gt 0) {
        foreach ($hint in $mouseContext.SoftwareHints) {
            $state = if ($hint.Installed) { "installed" } else { "missing" }
            Write-Info "  $($hint.Vendor): $($hint.Software) ($state)"
        }
    } else {
        Write-Info "  No vendor-specific software detected automatically."
    }

    if (-not $Apply.IsPresent) {
        Write-Info ""
        Write-Info "Dry run only. Add -Apply to write settings."
        if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 0 }
        exit 0
    }

    $confirmText = @"
Apply Boat Eye calibration?

Mode: $($activeRecommendation.Source)

options.txt: $resolvedOptionsPath
standardsettings: $(if ([string]::IsNullOrWhiteSpace($resolvedStandardSettingsPath)) { "(not found)" } else { $resolvedStandardSettingsPath })

Minecraft:
  mouseSensitivity -> $recommendedSensitivityText
  rawMouseInput    -> $rawInputTargetLabel

Windows:
  pointer speed    -> $($activeRecommendation.TargetCursorSpeed)
  disable accel    -> $disableMouseAccel

Visual Effects:
  apply            -> $($ApplyVisualEffects.IsPresent)
  screenEffectScale -> $(Clamp-Double $TargetScreenEffectScale 0.0 1.0)
  fovEffectScale    -> $(Clamp-Double $TargetFovEffectScale 0.0 1.0)

DPI:
  current          -> $CurrentDpi
  target           -> $($activeRecommendation.TargetDpiRounded)
  NOTE: DPI is not changed by this script.
        Set DPI manually in your mouse software.
"@

    if (-not $NoPopup.IsPresent) {
        $ok = Show-ConfirmPopup -title "Boat Eye Calibration" -message $confirmText
        if (-not $ok) {
            $resultPayload.apply.canceled = $true
            $resultPayload.apply.message = "Apply canceled."
            Write-Info "Apply canceled."
            if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 0 }
            exit 0
        }
    }

    $backupDir = Join-Path $env:LOCALAPPDATA "Toolscreen\\backups"
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    $backupPath = Join-Path $backupDir ("boat_eye_mouse_backup_" + (Get-Date -Format "yyyyMMdd_HHmmss") + ".json")

    $backup = [PSCustomObject]@{
        created_utc = (Get-Date).ToUniversalTime().ToString("o")
        options_path = $resolvedOptionsPath
        standardsettings_path = $resolvedStandardSettingsPath
        before = [PSCustomObject]@{
            mouseSensitivity = $currentSensitivity
            sensitivitySource = $currentSensitivitySource
            optionsSensitivity = if ($hasOptionsSensitivity) { $optionsSensitivity } else { $null }
            standardSettingsSensitivity = if ($hasStandardSettingsSensitivity) { $standardSettingsSensitivity } else { $null }
            screenEffectScale = $screenEffectScaleCurrent
            fovEffectScale = $fovEffectScaleCurrent
            rawMouseInput = $rawInputCurrent
            dpi = $CurrentDpi
            windowsPointerSpeed = $windowsBefore.PointerSpeed
            windowsMouseSpeed = $windowsBefore.MouseSpeed
            windowsThreshold1 = $windowsBefore.Threshold1
            windowsThreshold2 = $windowsBefore.Threshold2
            windowsAccelDisabled = $windowsBefore.AccelDisabled
            nbbRegistrySensitivity = Get-NbbRegistryDouble -name "sensitivity"
        }
        target = [PSCustomObject]@{
            recommendationMode = $RecommendationMode
            inputMode = $InputMode
            recommendationSource = $activeRecommendation.Source
            dpi = $activeRecommendation.TargetDpiRounded
            windowsPointerSpeed = $activeRecommendation.TargetCursorSpeed
            currentCursorSpeedForCalc = $currentCursorSpeedForCalc
            currentCursorSource = $currentCursorSource
            includeCursorInRanking = $includeCursorInRanking
            preferHigherDpi = $preferHigherDpi
            maxRecommendedPixelSkipping = $MaxRecommendedPixelSkipping
            disableMouseAccel = $disableMouseAccel
            enableRawInput = $enableRawInput
            applyVisualEffects = $ApplyVisualEffects.IsPresent
            targetScreenEffectScale = (Clamp-Double $TargetScreenEffectScale 0.0 1.0)
            targetFovEffectScale = (Clamp-Double $TargetFovEffectScale 0.0 1.0)
            recommendedSensitivity = $recommendedSensitivity
            recommendedSensitivityAlt = $recommendedSensitivityAlt
            rawSensitivity = $activeRecommendation.RawSensitivity
            estimatedPixelSkipping = $activeRecommendation.EstimatedPixelSkipping
            preferredStandardSensitivity = if ($PreferredStandardSensitivity -ge 0.0) {
                (Clamp-Double $PreferredStandardSensitivity 0.0 1.0)
            } else {
                $null
            }
        }
    }

    ($backup | ConvertTo-Json -Depth 8) | Out-File -LiteralPath $backupPath -Encoding utf8
    $resultPayload.apply.backupPath = $backupPath
    Write-Info "Backup written: $backupPath"

    try {
        Set-OptionValue $parsed.Lines "mouseSensitivity" $recommendedSensitivityText
        if ($enableRawInput) {
            Set-OptionValue $parsed.Lines "rawMouseInput" "true"
        }
        $visualScreenScale = Clamp-Double $TargetScreenEffectScale 0.0 1.0
        $visualFovScale = Clamp-Double $TargetFovEffectScale 0.0 1.0
        if ($ApplyVisualEffects.IsPresent) {
            Set-OptionValue $parsed.Lines "screenEffectScale" (Format-DoubleInvariant $visualScreenScale)
            Set-OptionValue $parsed.Lines "fovEffectScale" (Format-DoubleInvariant $visualFovScale)
        }
        Write-OptionsFile $resolvedOptionsPath $parsed.Lines
        Write-Info "Updated options.txt"

        $standardSettingsUpdated = $false
        if (-not [string]::IsNullOrWhiteSpace($resolvedStandardSettingsPath)) {
            try {
                if ($null -eq $standardSettingsParsed) {
                    $standardSettingsParsed = Parse-StandardSettingsFile -path $resolvedStandardSettingsPath
                }
                if ($null -eq $standardSettingsParsed -or $null -eq $standardSettingsParsed.Json) {
                    throw "standardsettings content was empty."
                }
                $updatedStandardSettings = Set-StandardSettingsSensitivity -jsonObj $standardSettingsParsed.Json -value $recommendedSensitivity
                if ($ApplyVisualEffects.IsPresent) {
                    $updatedStandardSettings | Add-Member -NotePropertyName "screenEffectScale" -NotePropertyValue ([double]$visualScreenScale) -Force
                    $updatedStandardSettings | Add-Member -NotePropertyName "fovEffectScale" -NotePropertyValue ([double]$visualFovScale) -Force
                }
                Write-StandardSettingsFile -path $resolvedStandardSettingsPath -jsonObj $updatedStandardSettings
                $standardSettingsUpdated = $true
                Write-Info "Updated standardsettings.json"
            } catch {
                Write-Info "Warning: failed to update standardsettings.json: $($_.Exception.Message)"
            }
        }

        Set-WindowsMouseSettings -pointerSpeedTarget $activeRecommendation.TargetCursorSpeed -disableAccel $disableMouseAccel
        Set-NbbRegistryDouble -name "sensitivity" -value $recommendedSensitivity
        $windowsAfter = Get-WindowsMouseSettings

        Write-Info ""
        Write-Info "Applied:"
        Write-Info "  Minecraft mouseSensitivity -> $recommendedSensitivityText"
        if ($standardSettingsUpdated) {
            Write-Info "  standardsettings sensitivity -> $recommendedSensitivityText"
        }
        if ($null -ne $recommendedSensitivityAlt) {
            Write-Info "  Alt valid sensitivity      -> $recommendedSensitivityAltText"
        }
        if ($enableRawInput) {
            Write-Info "  Minecraft rawMouseInput    -> true"
        }
        if ($ApplyVisualEffects.IsPresent) {
            Write-Info ("  screenEffectScale          -> " + (Format-DoubleInvariant $visualScreenScale))
            Write-Info ("  fovEffectScale             -> " + (Format-DoubleInvariant $visualFovScale))
        }
        Write-Info "  Windows pointer speed      -> $($windowsAfter.PointerSpeed)"
        Write-Info "  NBB registry sensitivity   -> $recommendedSensitivityText"
        Write-Info "  Estimated pixel skipping   -> $($activeRecommendation.EstimatedPixelSkipping)"
        if ($activeRecommendation.TargetDpiRounded -ne $CurrentDpi) {
            Write-Info "  DPI                        -> manual change required ($CurrentDpi -> $($activeRecommendation.TargetDpiRounded))"
        } else {
            Write-Info "  DPI                        -> unchanged ($CurrentDpi)"
        }

        Write-Info ""
        Write-Info "Done. Restart Minecraft for the settings to be fully consistent."

        if (-not $NoPopup.IsPresent) {
            $statusMsg = @"
Calibration applied.

Sensitivity: $recommendedSensitivityText
Windows speed: $($windowsAfter.PointerSpeed)
Raw input: $rawInputTargetLabel
Visual effects: $(if ($ApplyVisualEffects.IsPresent) { "screen=" + (Format-DoubleInvariant $visualScreenScale) + ", fov=" + (Format-DoubleInvariant $visualFovScale) } else { "unchanged" })
DPI: manual ($CurrentDpi -> $($activeRecommendation.TargetDpiRounded))
"@
            Show-StatusPopup -title "Boat Eye Calibration" -message $statusMsg -isError $false
        }

        $resultPayload.apply.applied = $true
        $resultPayload.apply.message = "Calibration applied."
        $resultPayload.apply.after = [PSCustomObject]@{
            windowsPointerSpeed = $windowsAfter.PointerSpeed
            windowsMouseSpeed = $windowsAfter.MouseSpeed
            windowsThreshold1 = $windowsAfter.Threshold1
            windowsThreshold2 = $windowsAfter.Threshold2
            windowsAccelDisabled = $windowsAfter.AccelDisabled
            minecraftSensitivity = $recommendedSensitivity
            rawMouseInput = if ($enableRawInput) { "true" } else { $rawInputCurrent }
            screenEffectScale = if ($ApplyVisualEffects.IsPresent) { $visualScreenScale } else { $screenEffectScaleCurrent }
            fovEffectScale = if ($ApplyVisualEffects.IsPresent) { $visualFovScale } else { $fovEffectScaleCurrent }
            standardSettingsUpdated = $standardSettingsUpdated
            standardSettingsPath = if ([string]::IsNullOrWhiteSpace($resolvedStandardSettingsPath)) { $null } else { $resolvedStandardSettingsPath }
            nbbRegistrySensitivity = Get-NbbRegistryDouble -name "sensitivity"
        }
    } catch {
        $msg = $_.Exception.Message
        Write-Info "Apply failed: $msg"
        if (-not $NoPopup.IsPresent) {
            Show-StatusPopup -title "Boat Eye Calibration" -message ("Apply failed: " + $msg) -isError $true
        }
        $resultPayload.ok = $false
        $resultPayload.apply.message = "Apply failed: $msg"
        if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 1 }
        throw
    }

    if ($script:EmitJsonOnly) { Emit-JsonAndExit $resultPayload 0 }
} catch {
    if ($script:EmitJsonOnly) {
        if ($null -eq $resultPayload) {
            $resultPayload = [PSCustomObject]@{
                ok = $false
                error = $_.Exception.Message
            }
        } else {
            $resultPayload.ok = $false
            $resultPayload.error = $_.Exception.Message
        }
        Emit-JsonAndExit $resultPayload 1
    }
    throw
}
