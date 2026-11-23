#Requires -Modules @{ ModuleName='Pester'; ModuleVersion='5.0.0' }
# The mock ACP agent records requests without consuming provider quota.

Describe 'Feature: retired terminal diagnostics' -Tag 'Feature' {
    BeforeAll {
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
        $fixture = (Resolve-Path (Join-Path $PSScriptRoot '..\fixtures\Mock-AcpInteractionAgent.ps1')).Path
        $script:requestLog = Join-Path $env:TEMP ("ite2e-autofix-routing-{0}.log" -f [guid]::NewGuid().ToString('N'))
        New-Item -ItemType File -Path $script:requestLog | Out-Null
        $invocation = "& '$($fixture.Replace("'", "''"))' -LogPath '$($script:requestLog.Replace("'", "''"))'"
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($invocation))
        $command = "pwsh -NoProfile -EncodedCommand $encoded"
        $script:app = Start-Terminal -Package (Get-ItTestPackage) -PassFre $true -State @{
            featureRolloutId = '{00000000-0000-0000-0000-000000000001}'
            sidebarRolloutVersion = 1
            sidebarRolloutAssignment = 'Sidebar'
            sidebarLayoutMigrationCompleted = $true
            sidebarIntroductionShown = $true
        } -Settings @{
            language = 'en-US'
            tabLayout = 'vertical'
            acpAgent = 'custom:autofix-routing-fixture'
            acpCustomCommand = $command
            acpModel = ''
            autoErrorDetectionEnabled = $true
            autoFixEnabled = $true
        }
        $script:targets = @()
        foreach ($label in @('A', 'B')) {
            $shell = New-WtTab -App $script:app -Command 'pwsh.exe -NoLogo -NoExit' -Title "autofix-routing-$label"
            Set-WtPaneFocus -App $script:app -SessionId $shell.session_id
            Open-AgentPane -App $script:app | Out-Null
            $agent = Wait-Until -TimeoutSec 30 -Because "helper for tab $label" -Condition {
                Get-AgentPaneSession -App $script:app -OwnerPaneSessionId $shell.session_id
            }
            Wait-AgentReady -App $script:app -PaneSessionId $agent.PaneSessionId -TimeoutSec 60 |
                Should -BeTrue
            $script:targets += [pscustomobject]@{ Shell = $shell; Agent = $agent }
        }

        function Get-RoutingPromptCount($Target) {
            $pattern = '\|session/prompt\|' + [regex]::Escape($Target.Agent.AcpSessionId) + '\|'
            @(Select-String -LiteralPath $script:requestLog -Pattern $pattern).Count
        }
    }
    AfterAll {
        if ($script:app) { Stop-Terminal -App $script:app }
        if ($script:requestLog -and (Test-Path -LiteralPath $script:requestLog)) {
            Remove-Item -LiteralPath $script:requestLog
        }
    }

    It 'Legacy settings cannot route shell failures to either agent tab' {
        foreach ($target in $script:targets) {
            Set-WtPaneFocus -App $script:app -SessionId $target.Shell.session_id
            $token = [guid]::NewGuid().ToString('N')
            $marker = "shell-failure-$token"
            # Emit the same command-finished mark as integrated shells.
            $command = "[Console]::Write(('{0}]133;D;1{1}' -f [char]27, [char]7)); throw ('shell-failure-' + '$token')"
            Invoke-RunCommand -App $script:app -SessionId $target.Shell.session_id -Command $command | Out-Null
            Wait-Until -TimeoutSec 20 -Because 'PowerShell displayed the error' -Condition {
                (Get-WtCapture -App $script:app -SessionId $target.Shell.session_id -MaxLines 30) -match
                    [regex]::Escape($marker)
            } | Out-Null
            Test-UiElementExists -App $script:app -Selector 'DiagnosticsButton' -TimeoutSec 1 | Should -BeFalse
        }
        foreach ($target in $script:targets) {
            (Get-RoutingPromptCount $target) | Should -Be 0
        }
    }
}
