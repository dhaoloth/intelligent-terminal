# SSH and agent workspace

## Application boundaries

`TerminalPage` owns each window's connection tree, tab navigation, and terminal views. The saved-host store owns connection definitions. The settings model owns appearance and terminal profiles. WTA owns the existing agent-session registry. A connection definition, a live connection, a terminal pane, and a provider session have independent identities and lifetimes.

Use the current ConPTY path for normal Windows, WSL, and SSH terminals and for native agent TUIs. Keep manually configured terminal profiles and the existing appearance model. Treat Termora as the reference for the SSH manager workflow, ZCode for provider and appearance settings, and VS Code for navigating from an agent to a subagent's activity.

## Connection tree and catalog

Mount the connection tree on the physical left of the main workspace. Its position and contents persist across tab selection and both tab orientations. Keep the existing tab layout and terminal content inside their current layout container. Host activation opens a separate SSH tab. Host properties and management actions are available from the tree. Keep editor drafts separate from selection changes.

Build on `SshHostStore` and its compare-and-replace writes. Evolve the catalog to give groups stable IDs and parent references, retain host IDs, and support empty groups, move, rename, search, and explicit import. Migrate existing slash-separated groups and preserve each host's authentication behavior. Store host settings and protected credential references. Keep passwords, key contents, discovery results, and active sessions outside catalog JSON.

Use one typed connection description for shell launch, discovery, agent launch, SFTP, and forwarding. Maintain the current Windows OpenSSH resolver and launch path for terminal sessions. Support OpenSSH aliases, interactive passwords, explicit identity files, and the user's SSH agent. Keep effective hostname, username, port, proxy, ordered jump hosts, forwarding, and host-key policy consistent across operations. Validate reference changes and reject jump cycles. Import with preview and collision handling.

For GUI SFTP, test whether a structured SFTP client can use OpenSSH's subsystem transport on Windows with usable trust and authentication flows. Evaluate a structured SSH library if auth callbacks or transport support require it. Keep the file manager responsible for browsing, operations, transfer progress and cancellation. Offer saved passwords only after verifying a protected storage and authentication path. Interactive authentication remains available through the ordinary terminal.

## Native agent discovery and launch

Model WSL distributions and SSH hosts as supported execution locations with equal priority. Discover Codex CLI, Claude Code, Hermes Agent, and Qwen Code using provider executable metadata and a bounded probe in the selected environment. Distinguish a missing CLI from a failed connection, unsupported shell, or unavailable credentials. Cancel stale probes after a target change. Discovery uses the same execution environment as launch and does not install packages.

Activating a discovered agent opens its native TUI in an ordinary terminal tab. Use PTY behavior from the existing terminal path. Pass launch data using argument handling appropriate to the specific transport and remote shell. Store an immutable connection snapshot or revision for each launch so changing a saved host cannot redirect an existing session or its resume action.

Reuse existing WSL probe and native launch seams where they fit. Carry SSH target identity in the native launch and session paths. The current ACP `AgentSource` understands Host and WSL. Explicit unsupported locations must fail closed before they can reach any legacy fallback. SSH credentials, agent-provider credentials, and local session-control capabilities remain separate.

## Active sessions and subagents

Extend the existing WTA session read model. Use terminal pane and tab identity for the live view, and qualify provider session IDs by provider and execution environment. The active-sessions panel identifies provider, target, window or background state, and activity. Show usage and cost only when available and distinguish occupied context from cumulative token counts. Use the optional and stale value handling in `tools/wta/src/usage.rs` where applicable.

Keep tab closure, explicit stop, terminal reattachment, and provider history resume distinct. Reuse `ContentManager` keep-running and reattachment when they fit the provider's native lifecycle. Its retained ConPTY is process-scoped. An SSH disconnect makes the remote process state unknown until fresh provider evidence is available. Validate lifecycle behavior independently for each supported provider and transport.

The optional Subagents tab lists running children for the current conversation. Selecting a child shows its output in place while keeping the parent TUI mounted. Back restores the parent view and focus. Completed children disappear. Show an unavailable state when a provider cannot expose child identity, activity, or output, and an empty list when it confirms there are no active children. Keep the existing Codex watcher filter for inherited child transcripts and add a separate child projection if a supported source is verified.

## Delivery and verification

1. **Permanent connection tree.** Reuse the current catalog and editor first. Verify host navigation and properties while switching tabs in both layouts, keyboard activation, correct tab titles, and preservation of unsaved edits. Extend the catalog and test migration, empty groups, second-window refresh, and edit conflicts.
2. **SSH management.** Add search, import, key management, proxies, jump hosts, tunnels, reconnect, and GUI SFTP behind the shared connection description. Verify authentication success and failure, host-key changes, existing aliases, transfer cancellation, and file names containing spaces or Unicode in authorized disposable environments.
3. **Native TUIs in WSL and SSH.** Validate all four CLIs in both execution environments. Exercise installation present and absent, unreachable targets, login-required states, noisy shell startup, input, and resize.
4. **Sessions and optional data.** Verify duplicate provider IDs across targets, two windows, tab moves, close and reattach, explicit stop, SSH loss, and app restart. For each provider that exposes child information, verify the active list, Back, and completion while viewing a child. Verify unavailable states independently.
5. **Appearance and cleanup.** Import selected Windows Terminal appearance fields and referenced schemes through the current settings model. Verify defaults, existing manual profiles, fonts, palettes, cursor and highlighting settings. Complete removal of terminal error analysis and `/fix` from Intelligent Terminal while preserving native TUI input.

Native Windows build and runtime checks require an authorized Windows test environment. Provider metrics, child output, reconnect behavior, and authentication flows require checks against representative installed versions. Source inspection establishes the integration seams and informs the test design.

## Source anchors

- Terminal layout and launch: `src/cascadia/TerminalApp/TerminalPage.xaml`, `TerminalPage.cpp`, `TerminalPage.SshHosts.cpp`, and `SshHostStore.cpp`.
- Live tab retention: `doc/specs/tab-keep-running.md` and `src/cascadia/TerminalApp/TabManagement.cpp`.
- Native session authority: `doc/specs/hybrid-agent-session-tracking.md`, `tools/wta/src/agent_source.rs`, `agent_sessions.rs`, `session_registry.rs`, and `session_watcher/mod.rs`.
- SSH workflow reference: `../termora/src/main/kotlin/app/termora/tree/NewHostTree.kt`, `plugin/internal/ssh/SSHHostOptionsPane.kt`, and `transfer/internal/sftp/SFTPTransferProtocolProvider.kt` under the same Kotlin package.
