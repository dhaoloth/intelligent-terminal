#Requires -Modules @{ ModuleName='Pester'; ModuleVersion='5.0.0' }
# Release checklist §3 "Shell integration" — shell prompt-start events are
# forwarded to the protocol, while command exit codes stay local to Terminal.
# Missing shell integration remains safe for a non-integrated cmd.exe pane.
#
# Deterministic: no agent/LLM involved — assert on the wtcli event stream + protocol liveness.
#   Invoke-Pester test/e2e/tests -Tag Feature

BeforeDiscovery { $script:Ready = [bool](Get-AppxPackage | Where-Object { $_.Name -like '*IntelligentTerminal*' }) }

Describe 'Feature §3 Shell integration' -Tag 'Feature' -Skip:(-not $script:Ready) {
    BeforeAll {
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
        # No agent is pinned; this suite never opens the agent pane, so the default avoids
        # an unnecessary copilot install/auth dependency (the pre-warmed helper is irrelevant here).
        $script:app = Start-Terminal -Package (Get-ItTestPackage) -PassFre $true
    }
    AfterAll { if ($script:app) { Stop-Terminal -App $script:app } }

    It 'PowerShell prompt-start is forwarded but command completion remains local' {
        $pane = Get-ActivePane -App $script:app
        $sid = $pane.session_id
        $listener = Start-WtEventListener -App $script:app
        try {
            $output = "shell-integration-$([guid]::NewGuid().ToString('N'))"
            Invoke-RunCommand -App $script:app -SessionId $sid -Command "echo $output" -SettleSec 8 | Out-Null
            Assert-Pane -App $script:app -SessionId $sid -Match $output -TimeoutSec 10
            $prompt = Wait-WtEvent -Listener $listener -TimeoutSec 20 -Predicate {
                $_.method -eq 'vt_sequence' -and "$($_.params.pane_id)" -eq "$sid" -and
                "$($_.params.sequence)" -eq 'osc:133;A'
            }
            $prompt.params.sequence | Should -Be 'osc:133;A'
            @(Get-WtEvents -Listener $listener -Predicate {
                $_.method -eq 'vt_sequence' -and "$($_.params.pane_id)" -eq "$sid" -and
                "$($_.params.sequence)" -match '(?i)^osc:133;D(?:;|$)'
            }) | Should -BeNullOrEmpty
        }
        finally { Stop-WtEventListener -Listener $listener }
    }

    It 'Missing shell integration is safe (a cmd.exe pane that fails does not crash the terminal)' {
        # cmd.exe has no OSC 133 shell integration, so failures here are NOT detected — the
        # contract is simply that this stays safe: no crash, the error still renders, and the
        # protocol surface keeps responding.
        $tab = New-WtTab -App $script:app -Command 'cmd.exe' -Title 'no-shellint'
        try {
            # Run the bad command, then on a SEPARATE line echo the errorlevel. cmd expands
            # %errorlevel% at PARSE time, so a single-line `badcmd & echo %errorlevel%` captures the
            # OLD value (0) — the echo must be its own command to see the failure's 9009.
            Send-WtInput -App $script:app -SessionId $tab.session_id -Text "nonexistentcmd$(Get-Random)"
            Send-WtKeys -App $script:app -SessionId $tab.session_id -Keys @('Enter')
            Send-WtInput -App $script:app -SessionId $tab.session_id -Text 'echo ITERR=%errorlevel%'
            Send-WtKeys -App $script:app -SessionId $tab.session_id -Keys @('Enter')
            # cmd sets errorlevel 9009 for an unrecognized command, so the pane prints "ITERR=9009".
            # Asserting that is deterministic AND locale-robust — unlike the localized "not
            # recognized" text, and unlike the command name (which the echoed input line contains
            # even if nothing ran). The echoed line shows "%errorlevel%" literally; only the
            # expanded OUTPUT is "ITERR=9009", so this also proves Enter actually executed it.
            Assert-Pane -App $script:app -SessionId $tab.session_id -Match 'ITERR=9009' -TimeoutSec 12
            # The protocol surface is still alive and the WT process did not crash.
            { Invoke-WtCli -App $script:app -Arguments @('list-panes') } | Should -Not -Throw
            (Get-Process -Id $script:app.Pid -ErrorAction SilentlyContinue) | Should -Not -BeNullOrEmpty
        }
        finally { Close-WtPane -App $script:app -SessionId $tab.session_id }
    }
}
