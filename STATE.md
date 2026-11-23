# State

Task: Adapt Intelligent Terminal into a Windows SSH manager and agent workspace.
Status: The SSH manager and native agent discovery checkpoint is published on `dev/dhaoloth/ssh-agent-workspace`. Windows CI and runtime verification are open.
Scope: Windows terminal and SSH management, local agents in WSL, and agents on remote SSH hosts.

## Verified

- Intelligent Terminal already opens OpenSSH aliases in ordinary terminal tabs. Its dynamic profile generator reads `Host` entries followed by `HostName` from Windows OpenSSH configuration and adds an SSH folder to the new-tab menu. The profile editor exposes the launch command, while OpenSSH applies connection settings and handles interactive authentication.
- The existing terminal tabs, sidebar, and profile settings are available to reuse. WTA also has detection and ACP launch for a selected WSL distribution. Detection currently targets built-in providers and needs to be checked against the four requested CLI agents.
- Termora implements a saved host tree with nested folders, search, tags, import, and host actions. Its SSH editor covers authentication, proxies, jump hosts, tunnels, terminal and SFTP settings. Its connection flows include host-key verification, reconnect, key management, and graphical SFTP.
- The working tree adds an SSH host tree docked on the left of the workspace, separate from tab selection. Host activation uses OpenSSH and ordinary terminal tabs. The pane exposes host creation, editing, and confirmed removal. The properties form keeps its draft separate from tree selection. OpenSSH prompts for passwords interactively.
- Host search matches name, address, or group and retains each matching host's folder ancestry. The query persists across tab changes and catalog refreshes.
- Explicit import previews literal aliases from the top-level Windows OpenSSH configuration files. Import writes host records atomically, skips aliases already in the catalog, and leaves existing OpenSSH settings authoritative for imported aliases. An offline OpenSSH configuration check confirmed the alias launch retains the configured port, user, jump host, identity, and host-key policy.
- The saved host editor accepts a literal OpenSSH jump alias for manually configured destinations. Shell tabs, native agent tabs, and discovery all pass the same jump alias. An offline `ssh -G` fixture verified destination and bastion configuration resolution.
- The connection tree includes registered WSL distributions. Selecting a distribution or saved SSH host invokes bounded native agent discovery. Detected Codex, Claude Code, Hermes, and Qwen agents get child rows that launch the native CLI in the selected environment. Source and synthetic shell tests cover target routing and probe output. No real WSL or SSH target was used for those tests.
- The fork stops creating new Azure Cloud Shell and Visual Studio profiles, retains compatible saved profiles, removes automatic Copilot onboarding and default selection, and removes the command palette's `?` delegation path and shortcut.
- Terminal-managed error analysis and the built-in `/fix` command are removed from the active product paths. Legacy settings remain readable. Shell integration for command history and session signals is available as an explicit installation action under AI Agents settings.
- Appearance settings can import portable fields from a chosen Windows Terminal `settings.json` into the fork's profile defaults. A separate GitHub Actions workflow builds an unsigned x64 MSIX using the existing package script.
- WSL checks covered source wiring, project and resource XML, and diff whitespace. Offline OpenSSH configuration checks covered interactive password and key modes, IPv6, key paths with spaces, and host-key verification settings.

## Decisions

- WSL and remote SSH hosts are equally important. Preserve ordinary Windows, WSL, and SSH terminal tabs. Local agents run in WSL distributions. Remote agents run on the selected SSH host.
- Use Termora as the reference for full SSH management in the UI, including the host tree and editable connection properties. Reuse matching Intelligent Terminal behavior, and choose the SSH backend based on the gaps. Reusing Termora source is optional.
- Keep the saved host tree docked on the left of the terminal workspace so it remains visible while switching tabs. Put host management actions on that pane. Activating a host opens a separate SSH terminal tab.
- Discover installed Codex CLI, Claude Code, Hermes Agent, and Qwen Code under each configured SSH host or WSL distribution. Selecting the host or distribution opens a terminal. Selecting an agent opens its native TUI in a separate application tab.
- Follow each agent's native session behavior. Closing a tab and stopping a task are separate actions. Show active sessions and their location in a dedicated panel. Show token usage and cost when the agent exposes them.
- Subagent inspection is optional per agent. When child sessions and their output are available, list active subagents in a tab inside the parent agent view. Selecting a subagent replaces the main view in place, Back returns to the main view, and completed subagents leave the active list. Show an unavailable state when the current agent cannot expose child sessions and their output.
- Preserve Intelligent Terminal's terminal experience and support carrying over the user's Windows Terminal font, colors, highlighting, and other appearance preferences. Use ZCode as a reference for provider and appearance settings.
- Remove unused automatic profile creation and bundled defaults such as Azure Cloud Shell and Visual Studio profiles and automatic Copilot setup. Remove the terminal's `?` delegation shortcut and its error analysis features, including `/fix`. Keep the agents' native capabilities available in their TUIs.

## Open

- Validate the Windows build, both tab layouts, narrow windows, keyboard interaction, host persistence, terminal launch and authentication, and legacy profile and onboarding migrations.
- Extend host management with broader import support, SSH agent and key management, editable proxy and multi-hop settings, tunnels, reconnect, and graphical SFTP. The first OpenSSH import reads literal `Host` aliases in the top-level files. Included files and wildcard patterns are open.
- Validate discovery and native TUI launch for the four CLIs on authorized WSL and SSH targets. Interactive SSH password login remains available for terminals. Password-only hosts need an auth bridge before background discovery can work with that login method.
- Add the shared session panel, provider-supported usage and subagent views, and unavailable states.
- Verify appearance import, diagnostics removal, and explicit shell integration setup on Windows.

## Evidence

- Intelligent Terminal baseline: `0fe42594d` on `main`.
- Termora reference checkout: `../termora` at `f0acbb4` on `2.x`.
- Current implementation: `src/cascadia/TerminalApp/TerminalPage.SshHosts.cpp` and `src/cascadia/TerminalApp/SshHostStore.cpp`.
- Technical concept: `doc/specs/ssh-agent-workspace.md`, based on Astra Medium's independent source review.
- Native discovery: `tools/wta/src/cli/native_probe.rs` and `src/cascadia/inc/WslDistributions.h`.
- [Windows PR CI run for `692d78655`](https://github.com/dhaoloth/intelligent-terminal/actions/runs/45180163240): Rust build and unit tests passed. C++ compilation reported WinRT namespace and const-access errors, addressed in `66110a55a`.
- Intelligent Terminal: `src/cascadia/TerminalSettingsModel/SshHostGenerator.cpp`, `src/cascadia/TerminalSettingsModel/CascadiaSettingsSerialization.cpp`, `src/cascadia/TerminalSettingsEditor/Profiles_Base.xaml`, `src/cascadia/TerminalApp/TabStrip.xaml`, and `tools/wta/src/agent_check.rs`.
- Termora: `../termora/src/main/kotlin/app/termora/tree/NewHostTree.kt`, `../termora/src/main/kotlin/app/termora/plugin/internal/ssh/SSHHostOptionsPane.kt`, and `../termora/src/main/kotlin/app/termora/transfer/internal/sftp/SFTPTransferProtocolProvider.kt`.

## Next

- Verify each published commit through the fork's Windows CI. Exercise the host and agent workflows in authorized Windows, WSL, and SSH test environments. Continue the SSH management and agent session work described in the technical concept.
