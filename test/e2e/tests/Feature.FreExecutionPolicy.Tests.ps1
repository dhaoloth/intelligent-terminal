#Requires -Modules @{ ModuleName='Pester'; ModuleVersion='5.0.0' }
# FRE must not change any installed PowerShell host's CurrentUser execution policy.
# The legacy diagnostics settings remain parseable but do not trigger shell setup.

BeforeDiscovery {
    Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
    $script:DevReady = $false
    try { $null = Resolve-ItApp -Package Dev -ErrorAction Stop; $script:DevReady = $true } catch { $script:DevReady = $false }
    $script:DevReady = $script:DevReady -and (Test-WinAppAvailable)
}

Describe 'Feature §0 FRE leaves PowerShell execution policies unchanged' -Tag 'Feature' -Skip:(-not $script:DevReady) {
    BeforeAll {
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
    }

    It 'Saving FRE with legacy diagnostic settings leaves CurrentUser policies unchanged' {
        $before = @(Get-WtExecutionPolicyState | Sort-Object Name | Select-Object Name, Value)
        $app = Start-Terminal -Package Dev -ShowFre -Settings @{
            acpAgent = 'copilot'
            agentSessionManagementEnabled = $false
            autoErrorDetectionEnabled = $true
            autoFixEnabled = $true
        }
        try {
            Invoke-UiElement -App $app -Selector 'NextButton' -TimeoutSec 15 | Out-Null
            Wait-UiElement -App $app -Selector 'SaveButton' -TimeoutSec 15 | Out-Null
            Invoke-UiElement -App $app -Selector 'SaveButton' -TimeoutSec 15 | Out-Null
            Test-Until -TimeoutSec 90 -IntervalSec 2 -Condition { Get-FreCompleted -App $app } |
                Should -BeTrue -Because 'the setup can finish without changing PowerShell execution policy'

            $after = @(Get-WtExecutionPolicyState | Sort-Object Name | Select-Object Name, Value)
            ($after | ConvertTo-Json -Compress) | Should -Be ($before | ConvertTo-Json -Compress)
        }
        finally { Stop-Terminal -App $app }
    }
}
