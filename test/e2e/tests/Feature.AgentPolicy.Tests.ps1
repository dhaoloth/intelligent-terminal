#Requires -Modules @{ ModuleName='Pester'; ModuleVersion='5.0.0' }
# Release checklist: §1 "Policy lock UI works" / §0 "FRE respects policy locks" — agent
# Group-Policy gates (AgentPolicy.h). Driven by the CurrentUser policy hive
# (Software\Policies\Microsoft\IntelligentTerminal), which the C++ reader honors as a fallback
# after HKLM — the same code path and value semantics as a machine GPO, without touching HKLM.
# Same approach as the WinPS execution-policy suite (Feature.FreExecutionPolicy.Tests.ps1).
#
# These are deterministic, LLM-free assertions for the supported provider and
# session-management policy gates.
#
# REQUIRES A WRITABLE POLICY HIVE: the `Software\Policies` subtree is ACL-restricted to
# administrators. Run test/e2e/tools/Enable-WtAgentPolicyTesting.ps1 ONCE (elevated) to grant
# the current user write on its own policy key; after that this suite runs non-elevated. When the
# hive isn't writable (or a machine GPO overrides it), Test-WtAgentPolicyControllable is $false
# and this suite SKIPS cleanly — non-elevated CI stays green, like Feature.FreExecutionPolicy.

BeforeDiscovery {
    # Needs the package + copilot (agent pane) + winapp (Open-AgentPane), AND the HKCU policy
    # hive must be the effective one (no machine GPO overriding it) for a deterministic verdict.
    Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
    $script:Ready = [bool]((Get-AppxPackage | Where-Object { $_.Name -like '*IntelligentTerminal*' }) -and (Get-Command copilot -ErrorAction SilentlyContinue) -and (Get-Command winapp -ErrorAction SilentlyContinue))
    $script:PolicyControllable = Test-WtAgentPolicyControllable
}

Describe 'Feature §1 agent Group Policy locks (AllowedAgents)' -Tag 'Feature' -Skip:(-not ($script:Ready -and $script:PolicyControllable)) {
    BeforeAll {
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
        # Allowlist contains ONLY a non-existent built-in id, so the user's copilot selection is
        # blocked (EffectiveAcpAgent collapses to empty, GlobalAppSettings.cpp:581-584) AND the
        # auto-detect fallback — which only picks a POLICY-ALLOWED agent (TerminalPage.cpp:1107-
        # 1116) — finds nothing installed/allowed. So the pane has no agent to launch and cannot
        # reach a connected ACP session. Deterministic regardless of which agents are installed,
        # because the only allowed id is fake.
        $script:policyState = Set-WtAgentPolicy -Policy @{ AllowedAgents = @('no-such-builtin-zzz') }
        $script:app = Start-Terminal -Package (Get-ItTestPackage) -PassFre $true -Settings @{ acpAgent = 'copilot' }
    }
    AfterAll {
        if ($script:app) { Stop-Terminal -App $script:app }
        if ($script:policyState) { Restore-WtAgentPolicy -State $script:policyState }
    }

    It 'A built-in agent blocked by AllowedAgents cannot connect (no allowed agent to launch)' {
        # With no allowed agent there is nothing to launch, so the agent pane cannot even open
        # (Open-AgentPane throws on timeout). This is the observable effect of the allowlist block.
        { Open-AgentPane -App $script:app -TimeoutSec 20 } |
            Should -Throw -Because 'AllowedAgents excludes the selected agent and no allowed fallback is installed, so the agent pane has nothing to launch'
        Test-AgentPaneOpen -App $script:app | Should -BeFalse
    }
}

Describe 'Feature §1 agent Group Policy locks (AllowCustomAgents)' -Tag 'Feature' -Skip:(-not ($script:Ready -and $script:PolicyControllable)) {
    BeforeAll {
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
        # Isolate AllowCustomAgents: the user picks a CUSTOM agent (custom: scheme), which the
        # AllowedAgents allowlist does NOT gate (EffectiveAcpAgent.cpp:568-578 — custom is checked
        # only against AllowCustomAgents). We additionally set AllowedAgents to a fake id purely to
        # remove the built-in auto-detect fallback, so the ONLY thing that can block the custom
        # agent is AllowCustomAgents=Blocked. With it blocked, EffectiveAcpAgent collapses to empty
        # and there is no allowed fallback → the pane cannot connect.
        $script:policyState = Set-WtAgentPolicy -Policy @{ AllowCustomAgents = 'Blocked'; AllowedAgents = @('no-such-builtin-zzz') }
        $script:app = Start-Terminal -Package (Get-ItTestPackage) -PassFre $true -Settings @{ acpAgent = 'custom:policytest'; acpCustomCommand = 'copilot --acp --stdio' }
    }
    AfterAll {
        if ($script:app) { Stop-Terminal -App $script:app }
        if ($script:policyState) { Restore-WtAgentPolicy -State $script:policyState }
    }

    It 'A custom agent blocked by AllowCustomAgents cannot connect' {
        # AllowedAgents (fake id) does not gate custom: agents, so a non-opening pane here is
        # attributable to AllowCustomAgents=Blocked specifically: the custom selection is dropped
        # and no allowed built-in fallback exists, leaving nothing to launch.
        { Open-AgentPane -App $script:app -TimeoutSec 20 } |
            Should -Throw -Because 'AllowCustomAgents=Blocked drops the custom selection and no allowed built-in fallback exists, so the agent pane has nothing to launch'
        Test-AgentPaneOpen -App $script:app | Should -BeFalse
    }
}

Describe 'Feature §0 agent Group Policy locks (AllowAgentSessionHooks, FRE)' -Tag 'Feature' -Skip:(-not ($script:Ready -and $script:PolicyControllable)) {
    BeforeAll {
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
        # Session-management hooks have no protocol-observable surface, so we assert the UI lock:
        # with the policy Blocked the FRE overlay renders the "managed by your organization"
        # notice next to the session-management control (FreOverlay.cpp SessionHooksPolicyNotice +
        # RS_ FreOverlay_PolicyLocked). Set policy BEFORE launching the FRE (cold start re-reads it).
        $script:policyState = Set-WtAgentPolicy -Policy @{ AllowAgentSessionHooks = 'Blocked' }
        $script:app = Start-TerminalFre -Package (Get-ItTestPackage)
    }

    AfterAll {
        if ($script:app) { Stop-Terminal -App $script:app }
        if ($script:policyState) { Restore-WtAgentPolicy -State $script:policyState }
    }

    It 'AllowAgentSessionHooks=Blocked disables the session toggle and shows the policy notice in the FRE' {
        Test-FreShowing -App $script:app | Should -BeTrue -Because 'the FRE overlay must be up to inspect its policy notices'
        # Session-management controls and their policy notices live on the FRE's second page.
        Invoke-UiElement -App $script:app -Selector 'NextButton' -TimeoutSec 10 | Out-Null
        # 1) SessionHooksPolicyNotice is the named "managed by your organization" element that
        #    renders next to the session-management toggle ONLY when the policy is locked.
        Test-UiElementExists -App $script:app -Selector 'SessionHooksPolicyNotice' -TimeoutSec 10 |
            Should -BeTrue -Because 'a Blocked AllowAgentSessionHooks policy must surface the managed-by-organization notice'
        # 2) …and the control itself is DISABLED (greyed / not settable) — the literal "locked
        #    controls are disabled" guarantee, read from winapp's UIA isEnabled.
        Test-UiElementEnabled -App $script:app -Selector 'SessionManagementToggle' |
            Should -BeFalse -Because 'a Blocked AllowAgentSessionHooks policy must DISABLE the session-management toggle'
    }
}

Describe 'Feature §0 agent Group Policy locks (AllowAutomaticApproval, FRE)' -Tag 'Feature' -Skip:(-not ($script:Ready -and $script:PolicyControllable)) {
    BeforeAll {
        Import-Module (Join-Path $PSScriptRoot '..\ItE2E\ItE2E.psd1') -Force
        $script:policyState = Set-WtAgentPolicy -Policy @{ AllowAutomaticApproval = 'Blocked' }
        $script:app = Start-Terminal -Package (Get-ItTestPackage) -ShowFre -Settings @{
            acpAgent = 'copilot'
            'agentPane.yoloMode' = $true
            agentSessionManagementEnabled = $false
        }
        Invoke-UiElement -App $script:app -Selector 'NextButton' -TimeoutSec 10 | Out-Null
    }
    AfterAll {
        if ($script:app) { Stop-Terminal -App $script:app }
        if ($script:policyState) { Restore-WtAgentPolicy -State $script:policyState }
    }

    It 'FRE hides policy-blocked automatic approval' {
        Test-UiElementExists -App $script:app -Selector 'AutomaticApprovalToggle' -TimeoutSec 1 |
            Should -BeFalse
        (Get-WtSetting -App $script:app -Key 'agentPane.yoloMode') |
            Should -BeFalse -Because 'startup policy normalization must clear the stored preference'
    }
}
