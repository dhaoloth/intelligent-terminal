#Requires -Modules @{ ModuleName='Pester'; ModuleVersion='5.0.0' }
# Real ConPTY/WT/helper/master/ACP boundaries, with a gated local agent instead of an LLM.

BeforeDiscovery {
    $script:QueueReady = [bool](Get-AppxPackage | Where-Object { $_.Name -like '*IntelligentTerminal*' })
}

Describe 'Feature Prompt Queue' -Tag 'Feature', 'PromptQueue' -Skip:(-not $script:QueueReady) {
    BeforeAll {
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
        $script:queuePackage = Get-ItTestPackage
        $script:queueFixture = (Resolve-Path (Join-Path $PSScriptRoot '..\fixtures\Mock-AcpQueueAgent.ps1')).Path
        $script:queueArtifacts = Join-Path $PSScriptRoot '..\artifacts\prompt-queue-fixtures'

        function Get-QueueRecords {
            param([string]$Kind)
            $records = @(Get-ChildItem -LiteralPath $script:recordDirectory -Filter '*.json' -File |
                    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json -Depth 64 } |
                    Sort-Object sequence)
            if ($Kind) { $records | Where-Object kind -eq $Kind }
            else { $records }
        }

        function Open-QueueGate {
            param([Parameter(Mandatory)][string]$Name)
            New-Item -ItemType File -Path (Join-Path $script:controlDirectory $Name) -Force | Out-Null
        }

        function Get-QueueText {
            # The helper uses the alternate screen: capture its current viewport,
            # not the helper's separately scrollable chat history.
            Get-WtCapture -App $script:app -SessionId $script:agentPaneId -MaxLines 100
        }

        function Get-QueueTextRegex {
            param([Parameter(Mandatory)][string]$Key, [int]$Count = -1)
            $pattern = Get-WtaLocalizedTextRegex -Key $Key
            if ($Count -ge 0) {
                $pattern = $pattern.Replace([regex]::Escape('%{count}'), [string]$Count)
            }
            $pattern
        }

        function Wait-QueueText {
            param([Parameter(Mandatory)][string]$Pattern)
            Wait-Until -TimeoutSec 20 -IntervalSec 0.2 -Because "expected queue pane text; artifacts: $script:runDirectory" -Condition {
                (Get-QueueText) -match $Pattern
            } | Out-Null
        }

        function Get-QueueToastRegex {
            '(?m)^\s*(?:[^\p{L}\p{N}\s]+\s*)?(?:' +
                (Get-QueueTextRegex -Key 'queue.enqueued') + ')\s*[.!。]?\s*$'
        }

        function Send-QueueInput {
            param([Parameter(Mandatory)][string]$Text)
            Send-WtInput -App $script:app -SessionId $script:agentPaneId -Text $Text
            Start-Sleep -Milliseconds 150
            Send-WtKeys -App $script:app -SessionId $script:agentPaneId -Keys @('Enter')
        }

        function Wait-QueueRecordCount {
            param([string]$Kind = 'prompt', [Parameter(Mandatory)][int]$Count)
            Wait-Until -TimeoutSec 25 -IntervalSec 0.2 -Because "$Count ACP $Kind records; $script:recordDirectory" -Condition {
                @(Get-QueueRecords -Kind $Kind).Count -ge $Count
            } | Out-Null
            @(Get-QueueRecords -Kind $Kind) | Should -HaveCount $Count
        }

        function Assert-QueuePromptCountStable {
            param([Parameter(Mandatory)][int]$Count, [double]$Seconds = 1)
            $clock = [System.Diagnostics.Stopwatch]::StartNew()
            do {
                @(Get-QueueRecords -Kind prompt) | Should -HaveCount $Count
                Start-Sleep -Milliseconds 100
            } while ($clock.Elapsed.TotalSeconds -lt $Seconds)
        }

        function Assert-QueueSize {
            param([Parameter(Mandatory)][int]$Count, [switch]$Paused)
            $key = if ($Count -eq 1) { 'queue.header_one' } else { 'queue.header' }
            $header = Get-QueueTextRegex -Key $key
            $anyCount = ((Get-QueueTextRegex -Key 'queue.header') + '|' +
                (Get-QueueTextRegex -Key 'queue.header_one')).Replace([regex]::Escape('%{count}'), '\d+')
            $expectedCount = $header.Replace([regex]::Escape('%{count}'), "$Count(?!\d)")
            Wait-Until -TimeoutSec 20 -IntervalSec 0.2 -Because "the pinned queue contains $Count requests (paused=$Paused)" -Condition {
                $viewport = Get-QueueText
                if ([string]::IsNullOrWhiteSpace($viewport)) { return $false }
                $headers = [regex]::Matches($viewport, $anyCount)
                if ($Count -eq 0) { return $headers.Count -eq 0 }
                $headers.Count -eq 1 -and $headers[0].Value -match $expectedCount
            } | Out-Null
        }

        function Send-QueueWindowKey {
            param([Parameter(Mandatory)][int]$Vk, [switch]$Alt, [switch]$Ctrl)
            # active-pane resolves the source shell even while the helper owns focus,
            # so Set-WtPaneFocus's active-pane poll cannot verify helper focus.
            Invoke-WtCli -App $script:app -Arguments @('focus-pane', '-t', $script:agentPaneId) | Out-Null
            Send-WtWindowKey -App $script:app -Vk $Vk -Alt:$Alt -Ctrl:$Ctrl -RequireForeground | Out-Null
        }

        function Assert-QueueControlsAbsent {
            (Get-QueueText) | Should -Not -Match '\[Alt\+[RSD]\b'
        }

        function Click-QueueCount {
            param([Parameter(Mandatory)][int]$Count)
            $key = if ($Count -eq 1) { 'queue.header_one' } else { 'queue.header' }
            $pattern = Get-QueueTextRegex -Key $key -Count $Count
            $lines = (Get-QueueText) -split '\r?\n'
            $hits = @(
                for ($row = 0; $row -lt $lines.Count; $row++) {
                    foreach ($match in [regex]::Matches($lines[$row], $pattern)) {
                        $prefix = $lines[$row].Substring(0, $match.Index)
                        [pscustomobject]@{ Row = $row; Column = $Host.UI.RawUI.LengthInBufferCells($prefix) + 1 }
                    }
                }
            )
            $hits | Should -HaveCount 1 -Because 'click the visible count, never guessed coordinates'
            Send-AgentMouseClick -App $script:app -PaneSessionId $script:agentPaneId `
                -Column $hits[0].Column -Row $hits[0].Row | Out-Null
        }

        function Wait-QueueDraft {
            param([Parameter(Mandatory)][string]$Pattern)
            Wait-QueueText ('(?m)^\s*[│║|]\s*>\s*' + $Pattern)
        }

        function Start-QueueScenario {
            param([switch]$HoldSession)
            # Keep paths inside PowerShell literals: custom-command argv currently splits
            # whitespace rather than decoding quotes, including on paths without spaces.
            $invocation = "& '{0}' -ControlDirectory '{1}' -RecordDirectory '{2}'" -f
                $script:queueFixture.Replace("'", "''"),
                $script:controlDirectory.Replace("'", "''"),
                $script:recordDirectory.Replace("'", "''")
            if ($HoldSession) { $invocation += ' -HoldSession' }
            $encoded = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($invocation))
            $command = "pwsh -NoLogo -NoProfile -EncodedCommand $encoded"
            $profileId = "{$([guid]::NewGuid())}"
            $settings = @{
                acpAgent = 'custom:queue-fixture'
                acpCustomCommand = $command
                acpModel = ''
                agentPanePosition = 'bottom'
                defaultProfile = $profileId
                profiles = @{
                    defaults = @{}
                    list = @(@{
                        guid = $profileId
                        name = 'ItE2E Queue PowerShell'
                        commandline = 'pwsh.exe -NoLogo'
                        startingDirectory = $script:runDirectory
                    })
                }
            }
            # Start-Terminal may fail after backing up configuration but before returning App.
            $recoveryApp = Resolve-ItApp -Package $script:queuePackage
            try {
                $script:app = Start-Terminal -Package $script:queuePackage -PassFre $true -Settings $settings
            }
            catch {
                Assert-WtPackageInactive -App $recoveryApp
                Restore-WtConfig -App $recoveryApp
                throw
            }
            $script:sourcePaneId = [string](Get-ActivePane -App $script:app).session_id
            Open-AgentPane -App $script:app | Out-Null
            # list-panes excludes helper panes, and the session registry is populated only
            # after ACP initialization. Resolve the startup identity from this app's helpers.
            $script:agentPaneId = Wait-Until -TimeoutSec 45 -Because 'the revealed helper pane' -Condition {
                foreach ($helperId in @(Get-DescendantWtaIds -RootPid $script:app.Pid)) {
                    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$helperId"
                    # PID-named logs can belong to an earlier process with a different role.
                    if ($process.CommandLine -notmatch '--connect-master\b') { continue }
                    $log = Get-ItLogText -App $script:app -Name "wta-main_helper-$helperId.log"
                    $ids = [regex]::Matches($log, 'seeded app_state\.pane_id from WT_SESSION pane_id=([a-fA-F0-9-]+)')
                    if ($ids.Count) {
                        $paneId = $ids[$ids.Count - 1].Groups[1].Value
                        if ((Get-WtPaneStatus -App $script:app -SessionId $paneId).state -match 'run') {
                            $script:helperLogName = "wta-main_helper-$helperId.log"
                            return $paneId
                        }
                    }
                }
            }
            Wait-QueueRecordCount -Kind initialize -Count 1
            if ($HoldSession) {
                Wait-QueueRecordCount -Kind session -Count 1
            }
            else {
                Wait-AgentReady -App $script:app -TimeoutSec 45 | Should -BeTrue
            }
            @(Get-QueueRecords -Kind prompt) | Should -HaveCount 0
        }

        function Start-HeldQueuePrompt {
            param([Parameter(Mandatory)][string]$Marker, [string]$Text = $Marker)
            Send-QueueInput $Text
            Wait-QueueRecordCount -Count 1
            Wait-QueueText ([regex]::Escape("START_$Marker"))
            (Get-QueueText) | Should -Not -Match (Get-QueueToastRegex)
        }
    }

    BeforeEach {
        $script:app = $null
        $script:agentPaneId = $null
        $script:helperLogName = $null
        $script:token = [guid]::NewGuid().ToString('N').ToUpperInvariant()
        $script:runDirectory = Join-Path $script:queueArtifacts $script:token
        $script:controlDirectory = Join-Path $script:runDirectory 'control'
        $script:recordDirectory = Join-Path $script:runDirectory 'records'
        New-Item -ItemType Directory -Path $script:controlDirectory, $script:recordDirectory -Force | Out-Null
        Write-ItLog "Queue fixture artifacts: $script:runDirectory"
    }

    AfterEach {
        try {
            if ($script:app -and $script:agentPaneId) {
                Get-QueueText | Set-Content -LiteralPath (Join-Path $script:runDirectory 'agent-pane.txt') -Encoding utf8
                @(Get-QueueRecords -Kind overlap) | Should -HaveCount 0
            }
        }
        finally {
            if ($script:app) { Stop-Terminal -App $script:app }
        }
    }

    It 'Queued user messages run once in submission order' {
        Start-QueueScenario
        $first = "QUEUE_HOLD_$script:token"
        $second = "QUEUE_HOLD_SECOND_$script:token"
        $third = "QUEUE_THIRD_$script:token"
        $top = "SCROLL_TOP_$script:token"
        $viewport = (Get-QueueText) -split '\r?\n'
        $columns = [Math]::Max(1, [int](($viewport | ForEach-Object Length | Measure-Object -Maximum).Maximum))
        $fillerCount = [int][Math]::Ceiling(($viewport.Count * $columns * 3) / 'SCROLL_FILLER '.Length)
        $firstText = "$first $top $(('SCROLL_FILLER ' * $fillerCount).Trim()) SCROLL_BOTTOM_$script:token"
        Start-HeldQueuePrompt $first -Text $firstText
        Send-QueueInput $second
        Send-QueueInput $third
        Wait-QueueText (Get-QueueTextRegex -Key 'queue.enqueued')
        Assert-QueueSize 2
        Assert-QueuePromptCountStable 1

        $beforeText = Get-QueueText
        $beforeText | Should -Not -Match ([regex]::Escape($top)) -Because 'the deterministic active transcript must overflow the chat viewport'
        $beforeScroll = $beforeText -split '\r?\n'
        # Locate the pinned count by content rather than padding-dependent offsets.
        $headerPattern = Get-QueueTextRegex -Key 'queue.header' -Count 2
        $headerRow = @(0..($beforeScroll.Count - 1) | Where-Object { $beforeScroll[$_] -match $headerPattern })[-1]
        $headerRow | Should -Not -BeNullOrEmpty
        $beforeText | Should -Not -Match ([regex]::Escape($second))
        $beforeText | Should -Not -Match ([regex]::Escape($third))
        Assert-QueueControlsAbsent
        Send-AgentMouseEvent -App $script:app -PaneSessionId $script:agentPaneId -Kind ScrollUp -Count 100 | Out-Null
        Wait-QueueText ([regex]::Escape($top))
        $afterScroll = (Get-QueueText) -split '\r?\n'
        $afterScroll[$headerRow] | Should -Be $beforeScroll[$headerRow] `
            -Because 'chat really scrolled to its hidden top, but the queue count stayed on the same row'
        Send-AgentMouseEvent -App $script:app -PaneSessionId $script:agentPaneId -Kind ScrollDown -Count 100 | Out-Null

        Open-QueueGate "$first.release"
        Wait-QueueRecordCount -Count 2
        Wait-QueueRecordCount -Kind completion -Count 1
        Assert-QueueSize 1
        Assert-QueuePromptCountStable 2 -Seconds 2
        (@(Get-QueueRecords -Kind prompt).marker -join '|') | Should -Be "$first|$second"
        Open-QueueGate "$second.release"
        Wait-QueueRecordCount -Count 3
        Wait-QueueRecordCount -Kind completion -Count 3
        $prompts = @(Get-QueueRecords -Kind prompt)
        ($prompts.marker -join '|') | Should -Be "$first|$second|$third"
        $prompts[0].text | Should -Match ('(?s)## User Request\r?\n' + [regex]::Escape($firstText) + '$')
        $prompts[1].text | Should -Match ('(?s)## User Request\r?\n' + [regex]::Escape($second) + '$')
        $prompts[2].text | Should -Match ('(?s)## User Request\r?\n' + [regex]::Escape($third) + '$')
        @($prompts.sessionId | Select-Object -Unique) | Should -HaveCount 1
        $completions = @(Get-QueueRecords -Kind completion)
        $completions[0].sequence | Should -BeLessThan $prompts[1].sequence
        $completions[1].sequence | Should -BeLessThan $prompts[2].sequence
        Wait-QueueText ([regex]::Escape("ACK_$third"))
        Assert-QueueSize 0
        Assert-QueuePromptCountStable 3
    }

    It 'Pending queue shows the count above input while a request waits' {
        Start-QueueScenario
        $first = "QUEUE_HOLD_$script:token"
        $second = "QUEUE_HOLD_SECOND_$script:token"
        Start-HeldQueuePrompt $first
        Assert-QueueSize 0

        Send-QueueInput $second
        Assert-QueueSize 1
        (Get-QueueText) | Should -Not -Match ([regex]::Escape($second))
        Assert-QueueControlsAbsent
        Assert-QueuePromptCountStable 1

        Open-QueueGate "$first.release"
        Wait-QueueRecordCount -Count 2
        (@(Get-QueueRecords -Kind prompt).marker -join '|') | Should -Be "$first|$second"
        Assert-QueueSize 0

        Open-QueueGate "$second.release"
        Wait-QueueRecordCount -Kind completion -Count 2
        Assert-QueueSize 0
        Assert-QueuePromptCountStable 2
    }

    It 'Queue recall shortcut is disabled while attachments remain intact' {
        Start-QueueScenario -AutomaticFix $false
        $first = "QUEUE_HOLD_$script:token"
        $second = "QUEUE_SECOND_$script:token"
        $last = "QUEUE_LAST_$script:token"
        Start-HeldQueuePrompt $first
        Send-QueueInput $second
        $clipboard = Get-ClipboardSnapshot
        try {
            Send-WtInput -App $script:app -SessionId $script:agentPaneId -Text "$last "
            Set-ClipboardImage
            Send-AgentAltV -App $script:app -PaneSessionId $script:agentPaneId | Out-Null
            Wait-QueueDraft ([regex]::Escape($last) + '\s+\[image:\s+image-\d+\.png\]')
            Send-WtKeys -App $script:app -SessionId $script:agentPaneId -Keys @('Enter')
        }
        finally { Restore-ClipboardSnapshot -Snapshot $clipboard }
        Assert-QueueSize 2
        Assert-QueueControlsAbsent
        Send-QueueWindowKey -Vk 0x52 -Alt
        Assert-QueueSize 2
        Wait-QueueDraft '[│║|]\s*$'

        $draft = 'KEEP_DRAFT'
        Send-WtInput -App $script:app -SessionId $script:agentPaneId -Text $draft
        Wait-QueueDraft ([regex]::Escape($draft))
        Send-QueueWindowKey -Vk 0x52 -Alt
        Assert-QueueSize 2
        Wait-QueueDraft ([regex]::Escape($draft))
        Send-AgentKey -App $script:app -PaneSessionId $script:agentPaneId -Key BSpace -Count $draft.Length | Out-Null
        Send-QueueWindowKey -Vk 0x52 -Alt
        Assert-QueueSize 2
        Wait-QueueDraft '[│║|]\s*$'
        Assert-QueuePromptCountStable 1

        Open-QueueGate "$first.release"
        Wait-QueueRecordCount -Count 3
        Wait-QueueRecordCount -Kind completion -Count 3
        $prompts = @(Get-QueueRecords -Kind prompt)
        ($prompts.marker -join '|') | Should -Be "$first|$second|$last"
        $prompts[2].text | Should -Match ([regex]::Escape($last))
        $images = @($prompts[2].images)
        $images | Should -HaveCount 1
        $images[0].mimeType | Should -Be 'image/png'
        $images[0].byteCount | Should -BeGreaterThan 8
        $images[0].signature | Should -Be '89504E470D0A1A0A'
        Assert-QueueSize 0
        Assert-QueuePromptCountStable 3 -Seconds 2
    }

    It 'Stopping a turn keeps explicit requests paused without recovery controls (<StopMethod>)' -ForEach @(
        @{ StopMethod = '/stop' }
        @{ StopMethod = 'Ctrl+C' }
    ) {
        Open-QueueGate 'hold-cancel'
        Start-QueueScenario
        $first = "QUEUE_HOLD_$script:token"
        $second = "QUEUE_SECOND_$script:token"
        $third = "QUEUE_THIRD_$script:token"
        $fresh = "QUEUE_FRESH_$script:token"
        Start-HeldQueuePrompt $first
        Send-QueueInput $second
        Send-QueueInput $third
        Assert-QueueSize 2
        if ($StopMethod -eq '/stop') { Send-QueueInput '/stop' }
        else { Send-QueueWindowKey -Vk 0x43 -Ctrl }
        Wait-QueueRecordCount -Kind cancel -Count 1
        Assert-QueueSize 2 -Paused
        Assert-QueueControlsAbsent
        Send-QueueInput $fresh
        Assert-QueueSize 3 -Paused
        Send-QueueWindowKey -Vk 0x53 -Alt
        Assert-QueueSize 3 -Paused
        Assert-QueuePromptCountStable 1
        @(Get-QueueRecords -Kind completion) | Should -HaveCount 0

        Open-QueueGate 'release-cancel'
        Wait-QueueRecordCount -Kind completion -Count 1
        Assert-QueueSize 3 -Paused
        Assert-QueuePromptCountStable 1 -Seconds 2
        Send-QueueWindowKey -Vk 0x53 -Alt
        Click-QueueCount 3
        Assert-QueueSize 3 -Paused
        Assert-QueueControlsAbsent
        Assert-QueuePromptCountStable 1 -Seconds 2
        (@(Get-QueueRecords -Kind prompt).marker -join '|') | Should -Be $first
        $completions = @(Get-QueueRecords -Kind completion)
        $completions[0].reason | Should -Be 'cancelled'
        $completions | Should -HaveCount 1
    }

    It 'Queue discard shortcut and count clicks preserve waiting work and draft' {
        Start-QueueScenario -AutomaticFix $false
        $first = "QUEUE_HOLD_$script:token"
        $second = "QUEUE_SECOND_$script:token"
        $draft = "QUEUE_DRAFT_$script:token"
        Start-HeldQueuePrompt $first
        Send-QueueInput $second
        Assert-QueueSize 1
        Send-WtInput -App $script:app -SessionId $script:agentPaneId -Text $draft
        Wait-QueueDraft ([regex]::Escape($draft))
        Send-QueueWindowKey -Vk 0x44 -Alt
        Click-QueueCount 1
        Assert-QueueSize 1
        Assert-QueueControlsAbsent
        Wait-QueueDraft ([regex]::Escape($draft))
        Assert-QueuePromptCountStable 1
        @(Get-QueueRecords -Kind cancel) | Should -HaveCount 0
        @(Get-QueueRecords -Kind completion) | Should -HaveCount 0
        Open-QueueGate "$first.release"
        Wait-QueueRecordCount -Kind completion -Count 2
        @(Get-QueueRecords -Kind completion)[1].reason | Should -Be 'end_turn'
        Wait-QueueDraft ([regex]::Escape($draft))
        Send-WtKeys -App $script:app -SessionId $script:agentPaneId -Keys @('Enter')
        Wait-QueueRecordCount -Count 3
        Wait-QueueRecordCount -Kind completion -Count 3
        (@(Get-QueueRecords -Kind prompt).marker -join '|') | Should -Be "$first|$second|$draft"
        Assert-QueuePromptCountStable 3 -Seconds 2
        @(Get-QueueRecords -Kind cancel) | Should -HaveCount 0
    }

    It 'Failed turns keep explicit requests paused without recovery controls' {
        Start-QueueScenario
        $first = "QUEUE_HOLD_$script:token"
        $fresh = "QUEUE_FRESH_$script:token"
        $draft = "QUEUE_DRAFT_$script:token"
        Start-HeldQueuePrompt $first
        Send-QueueInput "QUEUE_SECOND_$script:token"
        Assert-QueueSize 1
        Open-QueueGate "$first.fail"
        Wait-QueueRecordCount -Kind completion -Count 1
        Wait-QueueText 'QUEUE_FIXTURE_FAILURE'
        Assert-QueueSize 1 -Paused
        Assert-QueuePromptCountStable 1
        Send-QueueInput $fresh
        Assert-QueueSize 2 -Paused
        Assert-QueuePromptCountStable 1 -Seconds 2
        Send-WtInput -App $script:app -SessionId $script:agentPaneId -Text $draft
        Wait-QueueDraft ([regex]::Escape($draft))
        foreach ($vk in @(0x52, 0x53, 0x44)) {
            Send-QueueWindowKey -Vk $vk -Alt
        }
        Click-QueueCount 2
        Assert-QueueSize 2 -Paused
        Assert-QueueControlsAbsent
        Wait-QueueDraft ([regex]::Escape($draft))
        Assert-QueuePromptCountStable 1
        Send-WtKeys -App $script:app -SessionId $script:agentPaneId -Keys @('Enter')
        Assert-QueueSize 3 -Paused
        (@(Get-QueueRecords -Kind prompt).marker -join '|') | Should -Be $first
        Assert-QueuePromptCountStable 1 -Seconds 2
        @(Get-QueueRecords -Kind cancel) | Should -HaveCount 0
    }
}
