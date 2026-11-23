# State

Task: Adapt Intelligent Terminal into a Windows SSH manager and agent workspace.
Status: The SSH manager and native agent discovery checkpoint is published on `dev/dhaoloth/ssh-agent-workspace`. Windows CI and runtime verification are open.
Scope: Windows terminal and SSH management, local agents in WSL, and agents on remote SSH hosts.

## Verified

- Intelligent Terminal already opens OpenSSH aliases in ordinary terminal tabs. Its dynamic profile generator reads `Host` entries followed by `HostName` from Windows OpenSSH configuration and adds an SSH folder to the new-tab menu. The profile editor exposes the launch command, while OpenSSH applies connection settings and handles interactive authentication.
- Termora implements a saved host tree with nested folders, search, tags, import, and host actions. Its SSH editor covers authentication, proxies, jump hosts, tunnels, terminal and SFTP settings. Its connection flows include host-key verification, reconnect, key management, and graphical SFTP.
- The SSH host tree is docked on the left of the workspace and stays available when switching tabs. Host activation uses OpenSSH and ordinary terminal tabs. The pane exposes host creation, editing, and confirmed removal. The properties form keeps its draft separate from tree selection. OpenSSH prompts for passwords interactively.
- Host search matches name, address, or group and retains each matching host's folder ancestry. The query persists across tab changes and catalog refreshes.
- Explicit import previews literal aliases from Windows OpenSSH configuration and bounded `Include` traversal. Import writes host records atomically, skips aliases already in the catalog, and leaves existing OpenSSH settings authoritative for imported aliases. An offline OpenSSH configuration check confirmed the alias launch retains the configured port, user, jump host, identity, and host-key policy.
- The saved host editor accepts a literal OpenSSH jump alias for manually configured destinations. Shell tabs, native agent tabs, and discovery all pass the same jump alias. An offline `ssh -G` fixture verified destination and bastion configuration resolution.
- Host properties offer OpenSSH defaults and agent, an interactive password prompt, or a private key file. Saved local, remote, and dynamic forwards apply to ordinary SSH tabs. The SFTP action opens OpenSSH's interactive SFTP client with the selected host's authentication and jump settings. Tab restart reuses the original connection command line.
- Graphical SFTP opens a remote directory browser and downloads selected files through the selected OpenSSH connection. Its background transport uses the SFTP subsystem without a PTY. The GUI stages downloads beside the selected destination and publishes them without overwriting existing files.
- The connection tree includes registered WSL distributions. Selecting a distribution or saved SSH host invokes bounded native agent discovery. Detected Codex, Claude Code, Hermes, and Qwen agents get child rows that launch the native CLI in the selected environment. Source and synthetic shell tests cover target routing and probe output. No real WSL or SSH target was used for those tests.
- The Agent sessions panel tracks native agent terminals opened from Connections. It identifies target and window or background tab, and opens the same pane when activated. The terminal connection state is shown independently of provider state. SSH disconnection leaves remote status unknown. Provider usage, cost, and child sessions remain unavailable.
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
- Extend host management with key management, editable proxy and multi-hop settings, and graphical upload and remote file operations. Verify tunneling, reconnect, SFTP browsing and downloads on Windows. Alias import handles literal hosts and unconditional local includes with final-component globbing. Environment expansion, wildcard host expansion, and conditional include evaluation are open.
- Validate discovery and native TUI launch for the four CLIs on authorized WSL and SSH targets. Interactive SSH password login remains available for terminals. Password-only hosts need an auth bridge before background discovery can work with that login method.
- Validate session panel updates for window transfers, kept tabs, disconnect and restart on Windows. Add provider-supported usage and subagent views where verified sources expose them.
- Verify appearance import, diagnostics removal, and explicit shell integration setup on Windows.

## Evidence

- Intelligent Terminal baseline: `0fe42594d` on `main`.
- Termora reference checkout: `../termora` at `f0acbb4` on `2.x`.
- Current implementation: `src/cascadia/TerminalApp/TerminalPage.SshHosts.cpp` and `src/cascadia/TerminalApp/SshHostStore.cpp`.
- Technical concept: `doc/specs/ssh-agent-workspace.md`, based on Astra Medium's independent source review.
- Native discovery: `tools/wta/src/cli/native_probe.rs` and `src/cascadia/inc/WslDistributions.h`.
- [Windows PR CI run for `692d78655`](https://github.com/dhaoloth/intelligent-terminal/actions/runs/45180163240): Rust build and unit tests passed. C++ compilation reported WinRT namespace and const-access errors, addressed in `66110a55a`.
- [Windows PR CI run for `66110a55a`](https://github.com/dhaoloth/intelligent-terminal/actions/runs/45184793644): WindowsTerminal.exe linked and Rust passed. The C++ build job stopped when the settings test compared native and WinRT GUID types. The assertion now uses the WinRT type on both sides.
- [Windows PR CI run for `9de053839`](https://github.com/dhaoloth/intelligent-terminal/actions/runs/45190010294): Rust built and 3,145 WTA tests passed. One slash-command regression assertion and one XAML event visibility error were corrected after the run.
- [Windows PR CI run for `994fb7c8a`](https://github.com/dhaoloth/intelligent-terminal/actions/runs/45194123117): Rust build and tests passed. The C++ build reported session focus and OpenSSH path argument errors, corrected after the run.
- [x64 MSIX run for `9de053839`](https://github.com/dhaoloth/intelligent-terminal/actions/runs/45190010387): Release WTA built. The runner could not parse the LF-only package script. A Git checkout fixture verified the CRLF attribute correction.
- [x64 MSIX run for `994fb7c8a`](https://github.com/dhaoloth/intelligent-terminal/actions/runs/45194122948): The script parsed and reached MSBuild. Clean Release build exposed the missing direct MIDL generation dependency on `Host.Proxy.vcxproj` in `TerminalConnection.vcxproj`.
- [Windows PR CI run for `6ffc4c2f2`](https://github.com/dhaoloth/intelligent-terminal/actions/runs/45198926925): WindowsTerminal.exe linked with the SSH tree and SFTP browser. Rust unit results were 3,153 passed, zero failed, and six ignored, with security analysis completed. The C++ job stopped compiling the appearance test because two calls used the wrong WinRT types; the test calls have been corrected.
- [x64 MSIX run for `6ffc4c2f2`](https://github.com/dhaoloth/intelligent-terminal/actions/runs/45198926816): Attempt one built Release WTA and stopped during MSBuild after GitHub canceled the step. The log did not report a compiler error. Attempt two checks the same commit.
- SFTP transport source: `tools/wta/src/cli/native_sftp.rs` and `src/cascadia/TerminalApp/TerminalPage.Sftp.cpp`. A standalone Rust 1.93 Windows-target type check passed. A local OpenSSH subsystem fixture verified directory listing and its cap, downloading a name containing Cyrillic and a space, rejecting an existing local destination, and temporary-file cleanup after a missing remote file.
- Intelligent Terminal: `src/cascadia/TerminalSettingsModel/SshHostGenerator.cpp`, `src/cascadia/TerminalSettingsModel/CascadiaSettingsSerialization.cpp`, `src/cascadia/TerminalSettingsEditor/Profiles_Base.xaml`, `src/cascadia/TerminalApp/TabStrip.xaml`, and `tools/wta/src/agent_check.rs`.
- Termora: `../termora/src/main/kotlin/app/termora/tree/NewHostTree.kt`, `../termora/src/main/kotlin/app/termora/plugin/internal/ssh/SSHHostOptionsPane.kt`, and `../termora/src/main/kotlin/app/termora/transfer/internal/sftp/SFTPTransferProtocolProvider.kt`.

## Next

- Verify each published commit through the fork's Windows CI. Exercise the host and agent workflows in authorized Windows, WSL, and SSH test environments. Continue the SSH management and agent session work described in the technical concept.
