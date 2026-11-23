function Save-TelemetryOwnedProcesses {
    param([Parameter(Mandatory)]$App)
    if (-not $App.Launched) { throw 'Telemetry scenarios require a test-owned window.' }
    [void]$script:ownedPids.Add([int]$App.Pid)
    foreach ($processId in @(Get-DescendantWtaIds -RootPid ([int]$App.Pid))) {
        [void]$script:ownedPids.Add([int]$processId)
    }
    # Retain the master as well as the helper even if a later setup phase aborts.
    @($script:ownedPids) | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $script:root 'owned-processes.json')
}

function Stop-TelemetryOwnedTerminal {
    param([Parameter(Mandatory)]$App)
    if (-not $App.Launched) { throw 'Refusing to close a window not launched by this telemetry run.' }
    Save-TelemetryOwnedProcesses -App $App
    $helpers = if (Get-Process -Id $App.Pid -ErrorAction SilentlyContinue) { @(Get-AgentPaneSessions -App $App) } else { @() }
    foreach ($helper in $helpers) {
        [int]$helper.HelperProcessId | Should -BeIn @($script:ownedPids)
        Close-WtPane -App $App -SessionId $helper.PaneSessionId
    }
    Wait-Until -TimeoutSec 20 -Because 'fixture helpers retire while their COM host is still alive' -Condition {
        @($helpers | Where-Object { Get-Process -Id $_.HelperProcessId -ErrorAction SilentlyContinue }).Count -eq 0
    } | Out-Null
    # Stop only this window process's WTA descendants while its COM server is
    # still alive, so shutdown callbacks cannot activate a headless replacement.
    foreach ($processId in @(Get-DescendantWtaIds -RootPid ([int]$App.Pid))) {
        $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if ($process -and $process.Path -eq $App.WtaPath) {
            Stop-Process -Id $processId -Force -ErrorAction Stop
        }
    }
    Stop-Terminal -App $App -RestoreSettings $false
    Wait-Until -TimeoutSec 45 -Because 'the owned package finishes asynchronous shutdown before the next launch' -Condition {
        @(Get-WtProcessesForApp -App $App -IncludePackageExecutables).Count -eq 0
    } | Out-Null
}

function Invoke-TelemetryPhase {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][scriptblock]$Action)
    $phase = [ordered]@{ Name = $Name; StartedUtc = [DateTime]::UtcNow.ToString('o'); Data = $null; Error = $null; CleanupError = $null }
    try { $phase.Data = & $Action }
    catch { $phase.Error = $_.ToString(); $script:phaseErrors[$Name] = $_ }
    finally {
        $phase.EndedUtc = [DateTime]::UtcNow.ToString('o')
        $script:phases[$Name] = [pscustomobject]$phase
        try {
            if ($script:app) { Save-TelemetryOwnedProcesses -App $script:app }
        }
        catch {
            $phase.CleanupError = $_.ToString()
            if (-not $script:phaseErrors.ContainsKey($Name)) {
                $script:phaseErrors[$Name] = $_
                $phase.Error = $_.ToString()
            }
        }
        $script:phases[$Name] = [pscustomobject]$phase
        $script:phases | ConvertTo-Json -Depth 18 | Set-Content -LiteralPath (Join-Path $script:root 'phases.json')
    }
}

function Get-TelemetryPhaseEvents {
    param([Parameter(Mandatory)][string]$Phase, [string]$Name, [string]$Provider)
    if ($script:phaseErrors.ContainsKey($Phase)) { throw $script:phaseErrors[$Phase] }
    $bounds = $script:phases[$Phase]
    if (-not $bounds) { throw "Telemetry phase was not executed: $Phase" }
    $script:records | Where-Object {
        $time = ([DateTimeOffset]$_.Timestamp).UtcDateTime.Add($script:traceClockCorrection)
        $time -ge ([DateTimeOffset]$bounds.StartedUtc).UtcDateTime -and $time -le ([DateTimeOffset]$bounds.EndedUtc).UtcDateTime -and
        (-not $Name -or $_.Name -eq $Name) -and (-not $Provider -or $_.Provider -eq $Provider)
    }
}

function Initialize-TelemetryPhaseClock {
    param([Parameter(Mandatory)][string]$CaptureDirectory)
    # tracerpt can render a rounded local UTC offset. Calibrate against the
    # logger header's FILETIME rather than assuming its displayed offset is exact.
    [xml]$xml = Get-Content -LiteralPath (Join-Path $CaptureDirectory 'events.xml') -Raw
    $header = $xml.SelectSingleNode("//*[local-name()='Event'][*[local-name()='EventData']/*[local-name()='Data'][@Name='StartTime']]")
    if (-not $header) { throw 'ETL logger header is required for phase timing.' }
    $fileTime = [long]$header.SelectSingleNode("*[local-name()='EventData']/*[local-name()='Data'][@Name='StartTime']").InnerText
    $displayed = [DateTimeOffset]$header.SelectSingleNode("*[local-name()='System']/*[local-name()='TimeCreated']").GetAttribute('SystemTime')
    $script:traceClockCorrection = [DateTime]::FromFileTimeUtc($fileTime) - $displayed.UtcDateTime
    @{ correctionTicks = $script:traceClockCorrection.Ticks; headerFileTime = $fileTime } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $script:root 'phase-clock.json')
}

function Invoke-TelemetryConversation {
    foreach ($number in 1..2) {
        $marker = 'TELEMETRY_CHAT_' + [guid]::NewGuid().ToString('N')
        Clear-AgentInput -App $script:app -PaneSessionId $script:agent.PaneSessionId | Out-Null
        Send-AgentPrompt -App $script:app -PaneSessionId $script:agent.PaneSessionId -Text $marker | Out-Null
        Wait-Until -TimeoutSec 30 -Because 'the same ACP session completes a normal fixture prompt' -Condition {
            (Get-Content -LiteralPath $script:requestLog -Raw) -match
                ([regex]::Escape("telemetry-chat-complete|$($script:agent.AcpSessionId)|$marker"))
        } | Out-Null
        Assert-AgentPaneText -App $script:app -PaneSessionId $script:agent.PaneSessionId -Pattern "ACK:$marker" -TimeoutSec 15
        Start-Sleep -Milliseconds 300
    }

    @{ SessionId = $script:agent.AcpSessionId; ExplicitPrompts = 2 }
}

function Invoke-TelemetryProviderChanges {
    $changes = @(
        @{ Key = 'acpAgent'; Value = 'custom:pwsh'; Role = 'primary'; From = 'custom'; To = 'custom' }
        @{ Key = 'delegateAgent'; Value = 'claude'; Role = 'delegate'; From = 'custom'; To = 'claude' }
        @{ Key = 'delegateAgent'; Value = 'custom:cmd'; Role = 'delegate'; From = 'claude'; To = 'custom' }
        @{ Key = 'delegateAgent'; Value = 'custom:pwsh'; Role = 'delegate'; From = 'custom'; To = 'custom' }
        @{ Key = 'acpAgent'; Value = ''; Role = 'primary'; From = 'custom'; To = 'none' }
    )
    foreach ($change in $changes) {
        Set-WtSetting -App $script:app -Key $change.Key -Value $change.Value | Out-Null
        Start-Sleep -Seconds 2
        Save-TelemetryOwnedProcesses -App $script:app
    }
    $listener = Start-WtEventListener -App $script:app -WaitForReady
    try {
        $unchangedUtc = [DateTime]::UtcNow.ToString('o')
        Set-WtSetting -App $script:app -Key delegateAgent -Value 'custom:pwsh' | Out-Null
        Set-WtSetting -App $script:app -Key copyOnSelect -Value (-not [bool](Get-WtSettingsObject -App $script:app).copyOnSelect) | Out-Null
        Start-Sleep -Seconds 2
        @{
            Expected = $changes; UnchangedUtc = $unchangedUtc
        }
    }
    finally { Stop-WtEventListener -Listener $listener }
}

function Invoke-TelemetrySecondWindow {
    $before = @(Get-WtWindows -App $script:app)
    $beforeIds = @($before | ForEach-Object { [string]$_.window_id })
    $beforeIds | Should -Contain ([string]$script:app.WindowId)
    $hwnds = @(Get-WtWindowHwnds -App $script:app | ForEach-Object { [string]$_.hwnd })
    $secondId = $null
    try {
        # Use the shipped newWindow action through real input. Do not launch a
        # packaged EXE directly or use Start-Terminal's cold-start behavior.
        Send-WtWindowKey -App $script:app -Vk 0x4E -Ctrl -Shift -RequireForeground | Out-Null
        $secondId = Wait-Until -TimeoutSec 20 -Because 'Ctrl+Shift+N creates exactly one new packaged window' -Condition {
            $created = @(Get-WtWindows -App $script:app | Where-Object { [string]$_.window_id -notin $beforeIds })
            if ($created.Count -eq 1) { [string]$created[0].window_id }
        }
        $newHwnd = Wait-Until -TimeoutSec 15 -Because 'the second window belongs to the same owned process' -Condition {
            Get-WtWindowHwnds -App $script:app | Where-Object {
                [int]$_.pid -eq [int]$script:app.Pid -and [string]$_.hwnd -notin $hwnds
            } | Select-Object -First 1
        }
        $second = $script:app.PSObject.Copy()
        $second.Hwnd = $newHwnd.hwnd
        $second.WindowId = $secondId
        $activationUtc = [DateTime]::UtcNow.ToString('o')
        foreach ($context in @($script:app, $second, $script:app)) {
            Send-WtWindowKey -App $context -Vk 0x10 -RequireForeground | Out-Null
        }
        @(Get-WtWindows -App $script:app).Count | Should -Be ($before.Count + 1)
        Save-TelemetryOwnedProcesses -App $script:app
        @{ ProcessId = $script:app.Pid; SecondWindowId = $secondId; ActivationUtc = $activationUtc }
    }
    finally {
        if ($secondId) {
            foreach ($tab in @(Get-WtTabs -App $script:app -WindowId $secondId)) {
                foreach ($pane in @(Get-WtPanes -App $script:app -WindowId $secondId -TabId ([string]$tab.tab_id))) {
                    Close-WtPane -App $script:app -SessionId $pane.session_id
                }
            }
            Wait-Until -TimeoutSec 15 -Because 'only the second window closes before later settings phases' -Condition {
                $ids = @(Get-WtWindows -App $script:app | ForEach-Object { [string]$_.window_id })
                $secondId -notin $ids -and [string]$script:app.WindowId -in $ids
            } | Out-Null
        }
        Set-WtPaneFocus -App $script:app -SessionId $script:shell.session_id
        Set-WtWindowForeground -App $script:app | Out-Null
    }
}

function Invoke-TelemetryStartupCase {
    param([Parameter(Mandatory)][string]$Name, [hashtable]$Settings)
    if ($script:app) {
        Save-TelemetryOwnedProcesses -App $script:app
        Stop-TelemetryOwnedTerminal -App $script:app
        $script:app = $null
    }
    @(Get-WtProcessesForApp -App $script:target -IncludePackageExecutables) | Should -HaveCount 0
    $script:app = Start-Terminal -Package Dev -Backup $false -CleanSettings $false -PassFre $true -Settings $Settings
    Save-TelemetryOwnedProcesses -App $script:app
    $script:startupCases[$Name] = [int]$script:app.Pid
    Start-Sleep -Seconds 2
    @{ Name = $Name; ProcessId = $script:app.Pid }
}
