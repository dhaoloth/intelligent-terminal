#Requires -Modules @{ ModuleName='Pester'; ModuleVersion='5.0.0' }
# Release checklist §0 FRE — the FRE-overlay-specific agent-setup items that ARE automatable via
# winapp UIA but were previously left manual. The FRE's SECOND page (reached via NextButton) hosts
# the agent dropdown, the session-management toggle, and the pane-position picker, all as named
# XAML controls. Deterministic: assert on those controls /
# their rendered state — no agent/LLM involved.
#
# Not covered here (genuinely not cleanly UIA-observable, kept manual/UT):
#   * "Copilot without install" / "install failure messages" — need a destructive uninstalled/failed
#     CLI state to induce.

BeforeDiscovery {
    $script:Ready = [bool]((Get-AppxPackage | Where-Object { $_.Name -like '*IntelligentTerminal*' }) -and (Get-Command winapp -ErrorAction SilentlyContinue))
    $script:CopilotReady = [bool](Get-Command copilot -ErrorAction SilentlyContinue)
    $script:OpenCodeReady = [bool](Get-Command opencode -ErrorAction SilentlyContinue)
    # Non-Copilot built-ins surface in the FRE picker only when their CLI is installed.
    $script:NonCopilot = @(
        @{ Cmd = 'claude'; Label = 'Claude' }
        @{ Cmd = 'codex';  Label = 'Codex' }
        @{ Cmd = 'gemini'; Label = 'Gemini' }
    ) | Where-Object { Get-Command $_.Cmd -ErrorAction SilentlyContinue }
    $script:HasNonCopilot = [bool]$script:NonCopilot
}

Describe 'Feature §0 FRE agent setup (overlay controls)' -Tag 'Feature' -Skip:(-not $script:Ready) {
    BeforeAll {
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
        $script:app = Start-TerminalFre -Package (Get-ItTestPackage)
        # Advance to the settings page (agent dropdown / preferences / position live here).
        Invoke-UiElement -App $script:app -Selector 'NextButton' -TimeoutSec 10 | Out-Null
        Start-Sleep -Seconds 1
        # Locale-robust "(installed)" suffix from the FreOverlay_AgentStatusInstalled resource, so
        # the install-state assertions work on non-en-US builds (fall back to the en-US literal).
        $rx = Get-WtReswTextRegex -Key 'FreOverlay_AgentStatusInstalled'
        $script:InstalledSfx = if ($rx) { $rx -replace '^\(\?i\)', '' } else { '(\s*\(installed\))' }
    }
    AfterAll { if ($script:app) { Stop-Terminal -App $script:app } }

    It 'Copilot preinstalled: the FRE agent dropdown shows Copilot as installed' -Skip:(-not $script:CopilotReady) {
        Invoke-UiElement -App $script:app -Selector 'AgentComboBox' -TimeoutSec 10 | Out-Null
        try {
            $shown = Test-Until -TimeoutSec 10 -IntervalSec 1 -Condition {
                (Get-UiTree -App $script:app -Depth 18) -match ("(?i)copilot[^\r\n]*" + $script:InstalledSfx)
            }
            $shown | Should -BeTrue -Because 'with the Copilot CLI installed, the FRE agent picker must list it as installed'
        }
        finally { Send-WtWindowKey -App $script:app -Vk 0x1B -RequireForeground | Out-Null }
    }

    It 'Setup hints are not rendered inside setting cards' {
        foreach ($hint in @('AgentInstallHintRow', 'AutoDetectShellIntegrationHintRow', 'SessionManagementHintRow')) {
            Test-UiElementExists -App $script:app -Selector $hint -TimeoutSec 1 |
                Should -BeFalse -Because "the FRE should not render the $hint inline hint"
        }
    }

    It 'Terminal diagnostics controls are absent from FRE' {
        foreach ($selector in @('ErrorDetectionComboBox', 'AutoDetectToggle', 'AutoErrorToggle')) {
            Test-UiElementExists -App $script:app -Selector $selector -TimeoutSec 1 |
                Should -BeFalse -Because "$selector is no longer part of first-run setup"
        }
    }

    It 'Display preferences are Settings-only' {
        foreach ($selector in @('TabModeComboBox', 'RenderAgentMarkdownToggle', 'ShowTokenUsageAndCostToggle')) {
            Test-UiElementExists -App $script:app -Selector $selector -TimeoutSec 1 |
                Should -BeFalse -Because 'advanced display preferences belong in normal Settings, not FRE'
        }
    }

    It 'FRE configures automatic approval' {
        Test-UiElementExists -App $script:app -Selector 'AutomaticApprovalToggle' -TimeoutSec 8 |
            Should -BeTrue -Because 'FRE must expose automatic approval for the supported default provider'
        Test-UiElementEnabled -App $script:app -Selector 'AutomaticApprovalToggle' |
            Should -BeTrue
        (Get-UiElement -App $script:app -Selector 'AutomaticApprovalToggle').toggleState |
            Should -Be 'off' -Because 'automatic approval defaults off'

        foreach ($key in @('AIAgents_YoloMode.Header', 'AIAgents_YoloMode.HelpText')) {
            $values = @(Get-WtReswTextValues -Key $key)
            $element = Wait-Until -TimeoutSec 12 -Because "FRE to render the shared $key resource" -Condition {
                foreach ($value in $values) {
                    if ($match = Get-UiElement -App $script:app -Selector $value) {
                        return $match
                    }
                }
            }
            $element.name | Should -BeIn $values
        }
    }

    It 'Non-Copilot agents appear as installed in the FRE agent picker' -Skip:(-not $script:HasNonCopilot) {
        # Expand the dropdown so all agent entries (not just the selected one) are in the tree,
        # then assert each installed non-Copilot CLI is offered and labelled installed.
        Invoke-UiElement -App $script:app -Selector 'AgentComboBox' -TimeoutSec 10 | Out-Null
        Start-Sleep -Milliseconds 800
        $tree = Get-UiTree -App $script:app -Depth 18
        foreach ($a in $script:NonCopilot) {
            $tree | Should -Match ("(?i)$($a.Label)[^\r\n]*" + $script:InstalledSfx) -Because "the installed $($a.Label) CLI must appear as a selectable installed agent in the FRE"
        }
    }
}

Describe 'Feature §0 FRE unsupported automatic approval' -Tag 'Feature' -Skip:(-not ($script:Ready -and $script:OpenCodeReady)) {
    BeforeAll {
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
        $script:app = Start-Terminal -Package (Get-ItTestPackage) -ShowFre -Settings @{
            acpAgent = 'opencode'
            'agentPane.yoloMode' = $true
            autoErrorDetectionEnabled = $false
            agentSessionManagementEnabled = $false
        }
        Invoke-UiElement -App $script:app -Selector 'NextButton' -TimeoutSec 10 | Out-Null
    }
    AfterAll { if ($script:app) { Stop-Terminal -App $script:app } }

    It 'FRE hides unsupported automatic approval' {
        Test-UiElementExists -App $script:app -Selector 'AutomaticApprovalToggle' -TimeoutSec 1 |
            Should -BeFalse
        Invoke-UiElement -App $script:app -Selector 'SaveButton' -TimeoutSec 20 | Out-Null
        Wait-Until -TimeoutSec 30 -Because 'FRE to persist unsupported automatic approval off' -Condition {
            (Get-WtSetting -App $script:app -Key 'agentPane.yoloMode') -eq $false
        } | Out-Null
    }
}
