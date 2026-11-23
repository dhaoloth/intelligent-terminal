#Requires -Modules @{ ModuleName='Pester'; ModuleVersion='5.0.0' }
# PR #488 checks that an explicit delegate source controls where `wta delegate`
# launches the configured agent. TerminalPage derives that source from the
# profile's `commandPaletteAgent` setting when handling Alt+Shift+B.
# These cases drive the CLI directly and assert on the error rendered by the
# selected source. Both require a runnable WSL distro.

BeforeDiscovery {
    $script:Ready = [bool](
        (Get-AppxPackage | Where-Object { $_.Name -like '*IntelligentTerminal*' }) -and
        (Get-Command winapp -ErrorAction SilentlyContinue) -and
        (Get-Command wsl.exe -ErrorAction SilentlyContinue)
    )
}

Describe 'Feature profile-scoped delegate source' -Tag 'Feature' -Skip:(-not $script:Ready) {
    BeforeAll {
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force

        $script:app = $null
        $script:skipReason = $null

        $distroProbe = Invoke-Native -FilePath 'wsl.exe' -Arguments @(
            '-e', 'sh', '-lc', 'printf "%s" "${WSL_DISTRO_NAME:-}"'
        ) -TimeoutSec 45
        $script:distro = $distroProbe.StdOut.Trim()
        if ($distroProbe.ExitCode -ne 0 -or -not $script:distro) {
            $script:skipReason = 'no runnable default WSL distro is available'
        }
        elseif ($script:distro -match '["\r\n]') {
            $script:skipReason = 'the default WSL distro name cannot be represented safely in a test command line'
        }

        $script:app = Start-Terminal -Package (Get-ItTestPackage) -PassFre $true `
            -Settings @{ acpAgent = 'copilot'; delegateAgent = 'copilot' }

        # A random, never-installed name is absent from BOTH the Windows PATH and every
        # distro's PATH, so the "agent unavailable" branch fires deterministically.
        $script:bogusAgent = "ite2e-bogus-delegate-$(Get-Random -Maximum 999999)"

        # Launch a delegate and return the NEW tab created in this window.
        $script:RunDelegate = {
            param([string[]]$ExtraArgs = @())
            $wid = [string]$script:app.WindowId
            $before = @((Get-WtTabs -App $script:app -WindowId $wid).tab_id)
            Invoke-Wta -App $script:app -TimeoutSec 40 -Raw -Arguments (@(
                    'delegate', 'hi', '--agent', 'copilot --acp --stdio',
                    '--delegate-agent', $script:bogusAgent) + $ExtraArgs) | Out-Null
            $newTab = $null
            for ($i = 0; $i -lt 30 -and -not $newTab; $i++) {
                $newTab = @(Get-WtTabs -App $script:app -WindowId $wid) | Where-Object { $_.tab_id -notin $before } | Select-Object -First 1
                if (-not $newTab) { Start-Sleep -Milliseconds 500 }
            }
            $panes = if ($newTab) { @(Get-WtPanes -App $script:app -WindowId $wid -TabId ([string]$newTab.tab_id)) } else { @() }
            @{ Tab = $newTab; Panes = $panes }
        }
    }
    AfterAll { if ($script:app) { Stop-Terminal -App $script:app } }

    It 'An explicit WSL delegate source never falls back to the Windows host' {
        if ($script:skipReason) { Set-ItResult -Skipped -Because $script:skipReason; return }

        $d = & $script:RunDelegate -ExtraArgs @('--delegate-source', 'wsl', '--delegate-wsl-distro', $script:distro)
        $d.Tab | Should -Not -BeNullOrEmpty -Because 'even a doomed WSL launch must open a tab so the real error stays visible'

        # bash's own "not found" (the launch wraps the command in `exec`) proves it really
        # ran in the distro; Windows would say "cannot find the file specified".
        $sid = $d.Panes[0].session_id
        (Test-Until -TimeoutSec 20 -IntervalSec 1 -Condition {
                (Get-WtCapture -App $script:app -SessionId $sid -MaxLines 40) -match '(?i)bash:.*not found'
            }) | Should -BeTrue -Because 'a missing WSL delegate agent must surface the real in-distro error, not silently switch to the host'
    }

    It 'The default delegate source is never diverted to WSL' {
        if ($script:skipReason) { Set-ItResult -Skipped -Because $script:skipReason; return }

        # No --delegate-source => host. A runnable distro on this machine (proven by the
        # probe above) must not tempt the CLI into routing there.
        $d = & $script:RunDelegate
        $d.Tab | Should -Not -BeNullOrEmpty -Because 'the host delegate launch must still open a tab'

        $sid = $d.Panes[0].session_id
        (Test-Until -TimeoutSec 20 -IntervalSec 1 -Condition {
                (Get-WtCapture -App $script:app -SessionId $sid -MaxLines 40) -match '(?i)cannot find the file specified|is not recognized'
            }) | Should -BeTrue -Because 'the default launch must fail with the real Windows error, proving it never ran inside WSL bash'
    }
}
