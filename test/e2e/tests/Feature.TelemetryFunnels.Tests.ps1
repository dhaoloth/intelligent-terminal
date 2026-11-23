#Requires -Modules @{ ModuleName='Pester'; ModuleVersion='5.0.0' }
# One bounded UAC capture covers the complete deterministic scenario. ETW is
# decoded before assertions, so absent telemetry fails rather than being inferred
# from diagnostic logs. No model quota, synthetic host config, or product hooks.
Describe 'Feature: telemetry funnels' -Tag 'Feature', 'Telemetry' -Skip:($env:ITE2E_TELEMETRY -ne '1') {
    BeforeAll {
        $script:app = $null
        $script:target = $null
        $script:originalHashes = $null
        $script:policyTransaction = $null
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
        . (Join-Path $PSScriptRoot 'helpers\TelemetryTrace.ps1')
        . (Join-Path $PSScriptRoot 'helpers\TelemetryFunnels.Scenarios.ps1')
        . (Join-Path $PSScriptRoot 'helpers\TelemetryPolicy.ps1')
        $script:ownedPids = [Collections.Generic.HashSet[int]]::new()
        $script:phases = [ordered]@{}
        $script:phaseErrors = @{}
        $script:startupCases = @{}
        if ((Get-ItTestPackage) -ne 'Dev') { throw 'Telemetry PR validation requires explicitly selected Dev.' }
        if (-not $env:ITE2E_EXPECTED_WTA_SHA256 -or -not $env:ITE2E_EXPECTED_APP_SHA256) {
            throw 'Supply WTA and TerminalApp.dll SHA256 values from the exact-source build receipt.'
        }
        $script:target = Resolve-ItApp -Package Dev
        Stop-StaleItInstances -App $script:target
        (Get-FileHash -LiteralPath $script:target.WtaPath).Hash | Should -Be $env:ITE2E_EXPECTED_WTA_SHA256
        (Get-FileHash -LiteralPath (Join-Path $script:target.InstallLocation 'TerminalApp.dll')).Hash |
            Should -Be $env:ITE2E_EXPECTED_APP_SHA256
        $script:originalHashes = @{}
        foreach ($path in @($script:target.SettingsPath, $script:target.StatePath)) {
            if ((Test-Path "$path.e2ebak") -or (Test-Path "$path.e2ebak.missing")) { throw "Existing backup requires recovery: $path" }
            $script:originalHashes[$path] = if (Test-Path -LiteralPath $path) { (Get-FileHash -LiteralPath $path).Hash } else { $null }
        }
        $root = if ($env:ITE2E_ARTIFACT_ROOT) { $env:ITE2E_ARTIFACT_ROOT } else { Join-Path $PSScriptRoot '..\artifacts' }
        $script:root = [IO.Path]::GetFullPath((Join-Path $root ("telemetry-" + [guid]::NewGuid().ToString('N'))))
        New-Item -ItemType Directory -Path $script:root -Force | Out-Null
        foreach ($package in @(Get-AppxPackage | Where-Object {
            $_.Name -like '*IntelligentTerminal*' -and $_.PackageFamilyName -ne $script:target.Package
        })) {
            $other = Resolve-ItApp -Package $package.PackageFamilyName
            @(Get-WtProcessesForApp -App $other -IncludePackageExecutables) |
                Should -HaveCount 0 -Because 'HKCU policy changes must not affect another running Intelligent Terminal package'
        }
        $script:policyTransaction = Initialize-TelemetryPolicyTransaction -Directory $script:root
        $script:requestLog = Join-Path $script:root 'fixture.log'
        $script:telemetryFixture = Join-Path $script:root 'telemetry-fixture.json'
        @{ mode = 'chat' } | ConvertTo-Json | Set-Content -LiteralPath $script:telemetryFixture
        $fixture = (Resolve-Path (Join-Path $PSScriptRoot '..\fixtures\Mock-AcpInteractionAgent.ps1')).Path
        $invoke = "& '$($fixture.Replace("'", "''"))' -LogPath '$($script:requestLog.Replace("'", "''"))' -TelemetryFixturePath '$($script:telemetryFixture.Replace("'", "''"))'"
        $command = 'pwsh -NoProfile -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($invoke))
        $script:slashFailure = $null
        $trace = Start-TestTelemetryTrace -Directory (Join-Path $script:root 'capture')
        try {
            Stop-StaleItInstances -App $script:target
            foreach ($name in @('AllowedAgents', 'AllowCustomAgents')) {
                Set-TelemetryPolicy -Transaction $script:policyTransaction -Name $name -Value $null
            }
            $script:app = Start-Terminal -Package Dev -PassFre $true -State @{
                featureRolloutId = '{00000000-0000-0000-0000-000000000002}'
                sidebarRolloutVersion = 1; sidebarRolloutAssignment = 'Horizontal'
                sidebarLayoutMigrationCompleted = $true
            } -Settings @{
                acpAgent = 'custom:telemetry-fixture'; acpCustomCommand = $command; acpModel = ''
                delegateAgent = 'copilot'
                tabLayout = 'horizontal'; 'agentPane.yoloMode' = $false
            }
            $script:app.Launched | Should -BeTrue
            $script:shell = Get-ActivePane -App $script:app
            Open-AgentPane -App $script:app | Out-Null
            $script:agent = Wait-NewAgentPaneSession -App $script:app -OwnerPaneSessionId $script:shell.session_id -TimeoutSec 30
            Wait-AgentReady -App $script:app -PaneSessionId $script:agent.PaneSessionId -TimeoutSec 60 | Should -BeTrue
            $script:primaryAppPid = [int]$script:app.Pid
            $script:startupCases['initial'] = $script:primaryAppPid
            Save-TelemetryOwnedProcesses -App $script:app
            Set-WtPaneFocus -App $script:app -SessionId $script:shell.session_id
            Send-WtWindowKey -App $script:app -Vk 0x10 -RequireForeground | Out-Null
            $script:initialReadyUtc = [DateTime]::UtcNow
            @{
                appPid = $script:app.Pid; helperPid = $script:agent.HelperProcessId
                package = $script:target.Package; installLocation = $script:target.InstallLocation
                wtaSha256 = $env:ITE2E_EXPECTED_WTA_SHA256; appSha256 = $env:ITE2E_EXPECTED_APP_SHA256
            } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $script:root 'package.json')

            try {
                Clear-AgentInput -App $script:app -PaneSessionId $script:agent.PaneSessionId | Out-Null
                Invoke-AgentMenuItem -App $script:app -PaneSessionId $script:agent.PaneSessionId -Name '/config'
                Assert-AgentPaneText -App $script:app -PaneSessionId $script:agent.PaneSessionId -Pattern 'Mode' -TimeoutSec 15
                Send-AgentKey -App $script:app -PaneSessionId $script:agent.PaneSessionId -Key Escape | Out-Null
            }
            catch { $script:slashFailure = $_ }

            Invoke-TelemetryPhase -Name conversation -Action { Invoke-TelemetryConversation }
            Invoke-TelemetryPhase -Name second-window -Action { Invoke-TelemetrySecondWindow }
            @(Get-WtWindows -App $script:app) | Should -HaveCount 1 -Because 'later settings changes must only reach the retained primary window'
            Invoke-TelemetryPhase -Name provider-changes -Action { Invoke-TelemetryProviderChanges }
            if ($script:app) {
                Stop-TelemetryOwnedTerminal -App $script:app
                $script:app = $null
            }
            Invoke-TelemetryPhase -Name startup-inventory -Action {
                Invoke-TelemetryStartupCase -Name inventory -Settings @{
                    acpAgent = 'custom:pwsh'; acpCustomCommand = $command
                    acpCustomCommands = @($command, "$command -duplicate", 'cmd.exe /d /c exit 0')
                    delegateAgent = 'custom:cmd'; delegateCustomCommand = 'cmd.exe /d /c exit 0'
                    delegateCustomCommands = @('cmd.exe /d /c exit 0', 'pwsh -NoProfile -Command exit')
                    tabLayout = 'vertical'
                }
            }
            Invoke-TelemetryPhase -Name startup-no-agent -Action {
                Invoke-TelemetryStartupCase -Name noAgent -Settings @{
                    acpAgent = ''; acpCustomCommand = ''; acpCustomCommands = @()
                    delegateAgent = ''; delegateCustomCommand = ''; delegateCustomCommands = @()
                    tabLayout = 'horizontal'
                }
            }
            Invoke-TelemetryPhase -Name startup-policy-blocked -Action {
                Save-TelemetryOwnedProcesses -App $script:app
                Stop-TelemetryOwnedTerminal -App $script:app
                $script:app = $null
                Set-TelemetryPolicy -Transaction $script:policyTransaction -Name AllowedAgents -Value ([string[]]@())
                Set-TelemetryPolicy -Transaction $script:policyTransaction -Name AllowCustomAgents -Value 0
                Invoke-TelemetryStartupCase -Name policyBlocked -Settings @{
                    acpAgent = 'custom:pwsh'; acpCustomCommand = $command; acpCustomCommands = @('cmd.exe /d /c exit 0')
                    delegateAgent = 'custom:cmd'; delegateCustomCommand = 'cmd.exe /d /c exit 0'; delegateCustomCommands = @($command)
                    tabLayout = 'vertical'
                }
            }
            Invoke-TelemetryPhase -Name startup-policy-allowlist -Action {
                Save-TelemetryOwnedProcesses -App $script:app
                Stop-TelemetryOwnedTerminal -App $script:app
                $script:app = $null
                Set-TelemetryPolicy -Transaction $script:policyTransaction -Name AllowedAgents -Value ([string[]]@('copilot'))
                Set-TelemetryPolicy -Transaction $script:policyTransaction -Name AllowCustomAgents -Value 1
                Invoke-TelemetryStartupCase -Name policyAllowlist -Settings @{
                    acpAgent = 'custom:pwsh'; acpCustomCommand = $command; acpCustomCommands = @()
                    delegateAgent = 'claude'; delegateCustomCommand = ''; delegateCustomCommands = @()
                    tabLayout = 'horizontal'
                }
            }
            Invoke-TelemetryPhase -Name startup-not-enrolled -Action {
                Save-TelemetryOwnedProcesses -App $script:app
                Stop-TelemetryOwnedTerminal -App $script:app
                $script:app = $null
                @(Get-WtProcessesForApp -App $script:target) | Should -HaveCount 0
                Set-TelemetryPolicy -Transaction $script:policyTransaction -Name AllowedAgents -Value $null
                Set-TelemetryPolicy -Transaction $script:policyTransaction -Name AllowCustomAgents -Value $null
                # The original configuration backup still owns this inactive state.
                # A version-1 record without its GUID is rejected, not auto-enrolled
                # during settings load as an ordinary fresh version-0 state would be.
                $state = Get-WtStateObject -App $script:target
                $state.PSObject.Properties.Remove('featureRolloutId')
                $state.sidebarRolloutVersion = 1
                $state.sidebarRolloutAssignment = 'Horizontal'
                $state | ConvertTo-Json -Depth 64 | Set-Content -LiteralPath $script:target.StatePath -Encoding utf8
                Invoke-TelemetryStartupCase -Name notEnrolled -Settings @{
                    acpAgent = ''; acpCustomCommand = ''; acpCustomCommands = @()
                    delegateAgent = ''; delegateCustomCommand = ''; delegateCustomCommands = @()
                    tabLayout = 'vertical'
                }
                $loaded = Get-WtStateObject -App $script:app
                $loaded.PSObject.Properties.Name | Should -Not -Contain 'featureRolloutId'
                $loaded.sidebarRolloutVersion | Should -Be 1
                $loaded.sidebarRolloutAssignment | Should -Be 'Horizontal'
                (Get-WtSettingsObject -App $script:app).tabLayout | Should -Be 'vertical'
            }
        }
        finally { Stop-TestTelemetryTrace -Trace $trace }
        $script:records = @(Read-TestTelemetryTrace -Directory $trace.Directory -ProcessIds @($script:ownedPids) `
            -IncludeEventName @('AppCreated', 'AgentProviderConfigured', 'CustomAgentConfigured', 'SidebarStateOnLaunch',
                'AgentSlashCommandUsed', 'SlashCommandInvoked', 'SessionBecameInteractive', 'UserInteract',
                'ConnectionCreated', 'AcpNewSessionComplete', 'AgentPromptSent', 'AgentResponseComplete',
                'AgentProviderChanged', 'JsonSettingsChanged', 'AgentSessionStarted'))
        Initialize-TelemetryPhaseClock -CaptureDirectory $trace.Directory
        @($script:ownedPids) | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $script:root 'owned-processes.json')
        ConvertTo-Json -InputObject $script:records -Depth 12 | Set-Content -LiteralPath (Join-Path $script:root 'scoped-events.json')
    }

    AfterAll {
        try {
            if ($script:app -and $script:app.Launched) { Stop-TelemetryOwnedTerminal -App $script:app }
            if ($script:target -and $script:originalHashes) {
                if (@(Get-WtProcessesForApp -App $script:target -IncludePackageExecutables).Count) {
                    throw 'Selected package remains active; configuration backups retained rather than mutating a live user window.'
                }
                Restore-WtConfig -App $script:target
                foreach ($path in $script:originalHashes.Keys) {
                    $actual = if (Test-Path -LiteralPath $path) { (Get-FileHash -LiteralPath $path).Hash } else { $null }
                    $actual | Should -Be $script:originalHashes[$path] -Because 'settings and state must be restored byte-for-byte'
                }
            }
        }
        finally { if ($script:policyTransaction) { Restore-TelemetryPolicy -Transaction $script:policyTransaction } }
    }

    It 'Window startup telemetry has one consolidated typed snapshot' {
        $created = @($script:records | Where-Object {
            $_.ProcessId -eq $script:primaryAppPid -and $_.Name -eq 'AppCreated' -and
            ([DateTimeOffset]$_.Timestamp).UtcDateTime.Add($script:traceClockCorrection) -le $script:initialReadyUtc
        })
        $created | Should -HaveCount 1
        $expected = @{
            TabsInTitlebar = 'Boolean'
            PrimaryProvider = 'AnsiString'
            PrimaryEffectiveProvider = 'AnsiString'
            PrimaryCustomConfiguredCount = 'UInt32'
            PrimaryCustomSelectedCommandConfigured = 'Boolean'
            DelegateProvider = 'AnsiString'
            DelegateEffectiveProvider = 'AnsiString'
            DelegateCustomConfiguredCount = 'UInt32'
            DelegateCustomSelectedCommandConfigured = 'Boolean'
            AllowedAgentsPolicy = 'AnsiString'
            AllowCustomAgentsPolicy = 'AnsiString'
            SidebarEnabled = 'Boolean'
            SidebarView = 'AnsiString'
            SidebarRolloutName = 'UnicodeString'
            SidebarRolloutAssignment = 'UnicodeString'
            DefaultsFallback = 'Boolean'
        }
        @($created[0].Fields.Keys | Where-Object { $_ -ne 'PartA_PrivTags' } | Sort-Object) | Should -Be ($expected.Keys | Sort-Object)
        foreach ($field in $expected.Keys) {
            $created[0].Types[$field] | Should -Match ($expected[$field] + '$')
        }
        $created[0].Fields.PrimaryProvider | Should -Be 'custom'
        $created[0].Fields.SidebarRolloutName | Should -Be 'sidebar-upgrade-v1'
        $created[0].Fields.SidebarRolloutAssignment | Should -Be 'Horizontal'
        $created[0].Types.PartA_PrivTags | Should -Match 'UInt64$'
        $rejected = @(Get-TelemetryPhaseEvents -Phase startup-not-enrolled -Name AppCreated |
            Where-Object ProcessId -eq $script:startupCases.notEnrolled)
        $rejected | Should -HaveCount 1
        @($rejected[0].Fields.Keys | Where-Object { $_ -ne 'PartA_PrivTags' } | Sort-Object) | Should -Be ($expected.Keys | Sort-Object)
        foreach ($field in $expected.Keys) {
            $rejected[0].Types[$field] | Should -Match ($expected[$field] + '$')
        }
        $rejected[0].Fields.SidebarRolloutName | Should -Be ''
        $rejected[0].Fields.SidebarRolloutAssignment | Should -Be 'NotEnrolled'
        $rejected[0].Fields.SidebarEnabled | Should -BeIn @('true', '1')
        @($script:records | Where-Object Name -in @('AgentProviderConfigured', 'CustomAgentConfigured', 'SidebarStateOnLaunch')) | Should -HaveCount 0
    }

    It 'Agent slash telemetry uses only the renamed event' {
        if ($script:slashFailure) { throw $script:slashFailure }
        $used = @($script:records | Where-Object { $_.Provider -eq '4cfcff80-4e6b-5bfd-8ea1-d38e1226f70b' -and $_.Name -eq 'AgentSlashCommandUsed' })
        $used | Should -HaveCount 1
        $used[0].Fields.command | Should -Be 'config'
        $used[0].Types.command | Should -Match 'AnsiString$'
        @($script:records | Where-Object Name -eq 'SlashCommandInvoked') | Should -HaveCount 0
    }

    It 'Each same-process window emits one startup snapshot without activation duplicates' {
        $created = @(Get-TelemetryPhaseEvents -Phase second-window -Name AppCreated)
        $created | Should -HaveCount 1
        $created[0].ProcessId | Should -Be $script:primaryAppPid
        ([DateTimeOffset]$created[0].Timestamp).UtcDateTime.Add($script:traceClockCorrection) |
            Should -BeLessThan ([DateTimeOffset]$script:phases['second-window'].Data.ActivationUtc).UtcDateTime
        @($script:records | Where-Object { $_.ProcessId -eq $script:primaryAppPid -and $_.Name -eq 'AppCreated' }) |
            Should -HaveCount 2 -Because 'initial and secondary windows each emit once; activation and settings reload do not'
        @($created[0].Fields.Keys | Where-Object { $_ -ne 'PartA_PrivTags' }) | Should -HaveCount 16
        $created[0].Types.SidebarRolloutName | Should -Match 'UnicodeString$'
        $created[0].Types.SidebarRolloutAssignment | Should -Match 'UnicodeString$'
        $created[0].Fields.SidebarRolloutName | Should -Be 'sidebar-upgrade-v1'
        $created[0].Fields.SidebarRolloutAssignment | Should -Be 'Horizontal'
    }

    It 'Interactive sessions and connections expose local adoption sources' {
        $interactive = @($script:records | Where-Object { $_.ProcessId -eq $script:primaryAppPid -and $_.Name -eq 'SessionBecameInteractive' })
        $connections = @($script:records | Where-Object { $_.ProcessId -eq $script:primaryAppPid -and $_.Name -eq 'ConnectionCreated' })
        $interactive.Count | Should -BeGreaterThan 0
        $connections.Count | Should -BeGreaterThan 0
        foreach ($record in @($interactive) + @($connections)) {
            $record.Types.Count | Should -Be $record.Fields.Count
            $record.Types.Count | Should -BeGreaterThan 0
            $record.Types.PartA_PrivTags | Should -Match 'UInt64$'
        }
        $interactive[0].Fields.Branding | Should -Be '0'
        $interactive[0].Fields.Distribution | Should -Be '2'
        $interactive[0].Types.Branding | Should -Match 'UInt8$'
        $interactive[0].Types.Distribution | Should -Match 'UInt8$'
        $sourceConnections = @($connections | Where-Object { $_.Fields.SessionGuid.Trim('{}') -eq $script:shell.session_id.Trim('{}') })
        $sourceConnections | Should -HaveCount 1
        foreach ($field in @('ConnectionTypeGuid', 'ProfileGuid', 'SessionGuid')) {
            $sourceConnections[0].Types[$field] | Should -Match 'GUID$'
        }
        # ConnectionCreated reports Profile.ConnectionType, whose unset local
        # default is GUID_NULL; it does not report the ConPTY runtime class GUID.
        [guid]::Parse($sourceConnections[0].Fields.ConnectionTypeGuid) | Should -Be ([guid]::Empty)
        [guid]::Parse($sourceConnections[0].Fields.ProfileGuid) | Should -Not -Be ([guid]::Empty)
        [guid]::Parse($sourceConnections[0].Fields.SessionGuid) | Should -Be ([guid]$script:shell.session_id)
        foreach ($field in @('LaunchSource', 'ProfileKind', 'AgentId')) {
            $sourceConnections[0].Types[$field] | Should -Match 'AnsiString$'
        }
        $sourceConnections[0].Fields.LaunchSource | Should -BeExactly 'other'
        $sourceConnections[0].Fields.ProfileKind | Should -BeExactly 'shell'
        $sourceConnections[0].Fields.AgentId | Should -BeExactly 'none'
        $sourceConnections[0].Types.IsDefaultProfile | Should -Match 'Boolean$'
        $sourceConnections[0].Fields.IsDefaultProfile | Should -BeIn @('true', '1')
        @($sourceConnections[0].Fields.Keys | Sort-Object) |
            Should -Be @('AgentId', 'ConnectionTypeGuid', 'IsDefaultProfile', 'LaunchSource', 'PartA_PrivTags', 'ProfileGuid', 'ProfileKind', 'SessionGuid')
    }

    It 'ACP session creation and completed ordinary prompts expose engagement' {
        $created = @($script:records | Where-Object {
            $_.Name -eq 'AcpNewSessionComplete' -and $_.ProcessId -eq $script:agent.HelperProcessId
        })
        @($script:records | Where-Object {
            $_.Name -eq 'AcpNewSessionComplete' -and $_.Fields.Route -eq 'MasterForward' -and
            $_.Fields.Success -in @('true', '1')
        }).Count | Should -BeGreaterThan 0
        @($created | Where-Object { $_.Fields.Route -like 'Helper*' }).Count | Should -BeGreaterThan 0
        foreach ($record in $created) {
            $record.Fields.Keys | Should -Not -Contain 'SessionId'
            $record.Fields.Success | Should -BeIn @('true', '1')
            $record.Types.Success | Should -Match 'Boolean$'
            $record.Types.DurationMs | Should -Match 'Double$'
        }
        $prompts = @(Get-TelemetryPhaseEvents -Phase conversation -Name AgentPromptSent)
        $prompts | Should -HaveCount 2
        foreach ($record in $prompts) {
            $record.Types.SessionId | Should -Match 'AnsiString$'
            $record.Types.TurnId | Should -Match 'AnsiString$'
            [guid]::Parse($record.Fields.SessionId) | Should -Not -Be ([guid]::Empty)
            [guid]::Parse($record.Fields.TurnId) | Should -Not -Be ([guid]::Empty)
            $record.Fields.SessionId | Should -Not -Be $script:agent.AcpSessionId
            $record.Fields.TemplateKind | Should -Not -Be 'AgentCommand'
            $record.Types.PromptLengthBytes | Should -Match 'UInt32$'
            $record.Types.UserPromptOrdinal | Should -Match 'AnsiString$'
            $record.Fields.UserPromptOrdinal | Should -BeIn @('First', 'Second', 'Later')
        }
        $completed = @(Get-TelemetryPhaseEvents -Phase conversation -Name AgentResponseComplete | Where-Object {
            $_.ProcessId -eq $script:agent.HelperProcessId
        })
        $completed | Should -HaveCount 2
        foreach ($record in $completed) {
            $record.Fields.Success | Should -BeIn @('true', '1')
            $record.Types.TotalDurationMs | Should -Match 'Double$'
        }
        @($prompts.Fields.SessionId | Select-Object -Unique) | Should -HaveCount 1
        @($prompts.Fields.TurnId | Select-Object -Unique) | Should -HaveCount 2
        foreach ($sent in $prompts) {
            $result = @($completed | Where-Object { $_.Fields.TurnId -eq $sent.Fields.TurnId })
            $result | Should -HaveCount 1
            $result[0].Fields.SessionId | Should -Be $sent.Fields.SessionId
        }
    }

    It 'Daily keyboard activity telemetry deduplicates input across same-process windows' {
        $events = @($script:records | Where-Object { $_.ProcessId -eq $script:primaryAppPid -and $_.Name -eq 'UserInteract' })
        $events.Count | Should -BeGreaterThan 0
        foreach ($day in @($events | Group-Object { ([DateTimeOffset]$_.Timestamp).UtcDateTime.Add($script:traceClockCorrection).Date })) {
            $day.Count | Should -Be 1
        }
        foreach ($event in $events) {
            @($event.Fields.Keys | Sort-Object) | Should -Be @('Branding', 'Distribution', 'PartA_PrivTags')
            $event.Types.Branding | Should -Match 'UInt8$'
            $event.Types.Distribution | Should -Match 'UInt8$'
            $event.Fields.Branding | Should -Be '0'
            $event.Fields.Distribution | Should -Be '2'
        }
        @($script:records | Where-Object { $_.ProcessId -eq $script:primaryAppPid -and $_.Name -eq 'SessionBecameInteractive' }) | Should -HaveCount 1
        @(Get-TelemetryPhaseEvents -Phase second-window -Name AppCreated) | Should -HaveCount 1
    }

    It 'Provider changes expose bounded roles and transitions without unchanged reloads' {
        $actual = @(Get-TelemetryPhaseEvents -Phase provider-changes -Name AgentProviderChanged)
        $expected = @($script:phases['provider-changes'].Data.Expected)
        $actual | Should -HaveCount $expected.Count
        for ($i = 0; $i -lt $expected.Count; $i++) {
            $actual[$i].Fields.role | Should -Be $expected[$i].Role
            $actual[$i].Fields.from | Should -Be $expected[$i].From
            $actual[$i].Fields.to | Should -Be $expected[$i].To
            foreach ($field in @('role', 'from', 'to')) { $actual[$i].Types[$field] | Should -Match 'AnsiString$' }
        }
        @($actual | Where-Object {
            ([DateTimeOffset]$_.Timestamp).UtcDateTime.Add($script:traceClockCorrection) -ge
                ([DateTimeOffset]$script:phases['provider-changes'].Data.UnchangedUtc).UtcDateTime
        }) | Should -HaveCount 0
        @(Get-TelemetryPhaseEvents -Phase provider-changes -Name JsonSettingsChanged | Where-Object {
            ([DateTimeOffset]$_.Timestamp).UtcDateTime.Add($script:traceClockCorrection) -ge
                ([DateTimeOffset]$script:phases['provider-changes'].Data.UnchangedUtc).UtcDateTime
        }).Count | Should -BeGreaterThan 0 -Because 'an unrelated setting proves the unchanged-provider reload actually completed'
    }

    It 'Startup snapshots include inventory sidebar and no-agent states' {
        foreach ($phase in @('startup-inventory', 'startup-no-agent')) {
            @(Get-TelemetryPhaseEvents -Phase $phase -Name AppCreated) | Should -HaveCount 1
        }
        $inventory = @(Get-TelemetryPhaseEvents -Phase startup-inventory -Name AppCreated)[0]
        $none = @(Get-TelemetryPhaseEvents -Phase startup-no-agent -Name AppCreated)[0]
        foreach ($role in @('Primary', 'Delegate')) {
            $inventory.Fields["${role}Provider"] | Should -Be 'custom'
            $inventory.Fields["${role}EffectiveProvider"] | Should -Be 'custom'
            $inventory.Fields["${role}CustomConfiguredCount"] | Should -Be '2'
            $inventory.Fields["${role}CustomSelectedCommandConfigured"] | Should -BeIn @('true', '1')
            $none.Fields["${role}Provider"] | Should -Be 'none'
            $none.Fields["${role}EffectiveProvider"] | Should -Be 'none'
            $none.Fields["${role}CustomConfiguredCount"] | Should -Be '0'
            $none.Fields["${role}CustomSelectedCommandConfigured"] | Should -BeIn @('false', '0')
        }
        $inventory.Fields.SidebarEnabled | Should -BeIn @('true', '1')
        $none.Fields.SidebarEnabled | Should -BeIn @('false', '0')
        @($script:records | Where-Object { $_.ProcessId -eq $script:startupCases.noAgent -and $_.Name -eq 'AgentSessionStarted' }) |
            Should -HaveCount 0
        foreach ($snapshot in @($inventory, $none)) {
            @($snapshot.Fields.Keys | Where-Object { $_ -ne 'PartA_PrivTags' }) | Should -HaveCount 16
            $snapshot.Types.SidebarRolloutName | Should -Match 'UnicodeString$'
            $snapshot.Types.SidebarRolloutAssignment | Should -Match 'UnicodeString$'
            $snapshot.Fields.SidebarRolloutName | Should -Be 'sidebar-upgrade-v1'
            $snapshot.Fields.SidebarRolloutAssignment | Should -Be 'Horizontal'
            $snapshot.Fields.AllowedAgentsPolicy | Should -Be 'not_configured'
            $snapshot.Fields.AllowCustomAgentsPolicy | Should -Be 'not_configured'
            $snapshot.Fields.DefaultsFallback | Should -BeIn @('false', '0')
        }
    }

    It 'Startup policy buckets retain configured inventory and effective provider gates' {
        $blocked = @(Get-TelemetryPhaseEvents -Phase startup-policy-blocked -Name AppCreated)
        $allowlist = @(Get-TelemetryPhaseEvents -Phase startup-policy-allowlist -Name AppCreated)
        $blocked | Should -HaveCount 1
        $allowlist | Should -HaveCount 1
        $blocked[0].Fields.AllowedAgentsPolicy | Should -Be 'empty'
        $blocked[0].Fields.AllowCustomAgentsPolicy | Should -Be 'blocked'
        foreach ($role in @('Primary', 'Delegate')) {
            $blocked[0].Fields["${role}Provider"] | Should -Be 'custom'
            $blocked[0].Fields["${role}EffectiveProvider"] | Should -Be 'none'
            $blocked[0].Fields["${role}CustomConfiguredCount"] | Should -Be '2'
            $blocked[0].Fields["${role}CustomSelectedCommandConfigured"] | Should -BeIn @('true', '1')
        }
        $blocked[0].Fields.SidebarEnabled | Should -BeIn @('true', '1')
        @($script:records | Where-Object { $_.ProcessId -eq $script:startupCases.policyBlocked -and $_.Name -eq 'AgentSessionStarted' }) |
            Should -HaveCount 0
        $allowlist[0].Fields.AllowedAgentsPolicy | Should -Be 'allowlist'
        $allowlist[0].Fields.AllowCustomAgentsPolicy | Should -Be 'allowed'
        $allowlist[0].Fields.PrimaryProvider | Should -Be 'custom'
        $allowlist[0].Fields.PrimaryEffectiveProvider | Should -Be 'custom'
        $allowlist[0].Fields.DelegateProvider | Should -Be 'claude'
        $allowlist[0].Fields.DelegateEffectiveProvider | Should -Be 'none'
    }
}
