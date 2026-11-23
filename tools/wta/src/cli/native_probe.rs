//! Bounded one-shot discovery of native agent executables in WSL or over SSH.

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
#[cfg(windows)]
use std::path::PathBuf;
use std::{collections::BTreeMap, time::Duration};
use tokio::{io::AsyncReadExt, process::Command};

const PROBE_TIMEOUT: Duration = Duration::from_secs(12);
const CLEANUP_TIMEOUT: Duration = Duration::from_secs(2);
const MAX_STREAM_BYTES: usize = 8 * 1024;
const SENTINEL: &str = "WTA_NATIVE_AGENT_PROBE_V1:";
const REMOTE_SCRIPT: &str = r#"wta_location=${1:-ssh}
wta_probe() {
    p=$(command -v "$1" 2>/dev/null) || { printf false; return; }
    case "$p" in
        /*) ;;
        *) printf false; return ;;
    esac
    if [ "$wta_location" = wsl ]; then
        resolved=$(readlink -f "$p" 2>/dev/null) || resolved=$p
        case "$resolved" in
            /mnt/[a-zA-Z]/*|*.[eE][xX][eE]) printf false; return ;;
        esac
    fi
    case "$p" in
        *.[eE][xX][eE]) printf false ;;
        *) [ -f "$p" ] && [ -x "$p" ] && printf true || printf false ;;
    esac
}
printf 'WTA_NATIVE_AGENT_PROBE_V1:{"codex":%s,"claude":%s,"hermes":%s,"qwen":%s}\n' \
    "$(wta_probe codex)" "$(wta_probe claude)" "$(wta_probe hermes)" "$(wta_probe qwen)""#;

#[derive(Clone, Debug, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
enum Target {
    Wsl { distribution: String },
    Ssh { host: String },
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "snake_case")]
enum Status {
    Ok,
    TransportError,
    AuthError,
    IoError,
    Timeout,
    Unsupported,
    InvalidResponse,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct ProbeResult {
    schema_version: u8,
    target: Target,
    status: Status,
    agents: BTreeMap<&'static str, bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<&'static str>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RemoteAgents {
    codex: bool,
    claude: bool,
    hermes: bool,
    qwen: bool,
}

#[derive(Debug)]
struct Captured {
    bytes: Vec<u8>,
    overflow: bool,
}

fn result(
    target: Target,
    status: Status,
    error: Option<&'static str>,
    agents: Option<RemoteAgents>,
) -> ProbeResult {
    let mut available = BTreeMap::from([
        ("codex", false),
        ("claude", false),
        ("hermes", false),
        ("qwen", false),
    ]);
    if let Some(found) = agents {
        available.insert("codex", found.codex);
        available.insert("claude", found.claude);
        available.insert("hermes", found.hermes);
        available.insert("qwen", found.qwen);
    }
    ProbeResult {
        schema_version: 1,
        target,
        status,
        agents: available,
        error,
    }
}

pub(crate) async fn run(
    wsl_distro: Option<&str>,
    ssh_host: Option<&str>,
    ssh_hostname: Option<&str>,
    ssh_port: Option<u16>,
    ssh_user: Option<&str>,
    ssh_identity_file: Option<&str>,
    ssh_jump_alias: Option<&str>,
) -> Result<()> {
    let (target, mut command) = match (wsl_distro, ssh_host) {
        (Some(distro), None) => {
            if distro.is_empty() || distro.trim() != distro || distro.chars().any(char::is_control)
            {
                anyhow::bail!("--wsl-distro must be a non-empty exact distribution name");
            }
            let target = Target::Wsl {
                distribution: distro.to_owned(),
            };
            let mut command = match trusted_wsl_path() {
                Some(path) => Command::new(path),
                None => {
                    print_result(result(
                        target,
                        Status::Unsupported,
                        Some("trusted WSL executable is unavailable"),
                        None,
                    ));
                    return Ok(());
                }
            };
            command.args([
                "--distribution",
                distro,
                "--cd",
                "~",
                "--exec",
                "sh",
                "-lc",
                REMOTE_SCRIPT,
                "wta-native-agent-probe",
                "wsl",
            ]);
            (target, command)
        }
        (None, Some(host)) => {
            let host = validate_value("--ssh-host", host)?;
            let destination = ssh_hostname
                .map(|v| validate_value("--ssh-hostname", v))
                .transpose()?
                .unwrap_or(host);
            let jump_alias = ssh_jump_alias
                .map(|alias| validate_jump_alias(alias, host))
                .transpose()?;
            let target = Target::Ssh {
                host: host.to_owned(),
            };
            let mut command = match trusted_ssh_path() {
                Some(path) => Command::new(path),
                None => {
                    print_result(result(
                        target,
                        Status::Unsupported,
                        Some("trusted OpenSSH executable is unavailable"),
                        None,
                    ));
                    return Ok(());
                }
            };
            command.args([
                "-T",
                "-o",
                "BatchMode=yes",
                "-o",
                "ForwardAgent=no",
                "-o",
                "ConnectTimeout=5",
                "-o",
                "ConnectionAttempts=1",
                "-o",
                "ClearAllForwardings=yes",
                "-o",
                "PermitLocalCommand=no",
            ]);
            if let Some(alias) = jump_alias {
                command.args(["-J", alias]);
            }
            if let Some(port) = ssh_port {
                command.args(["-p".to_owned(), port.to_string()]);
            }
            if let Some(user) = ssh_user {
                command.args([
                    "-l".to_owned(),
                    validate_value("--ssh-user", user)?.to_owned(),
                ]);
            }
            if let Some(identity) = ssh_identity_file {
                let identity = validate_value("--ssh-identity-file", identity)?;
                command.args([
                    "-o",
                    "PreferredAuthentications=publickey",
                    "-o",
                    "IdentitiesOnly=yes",
                ]);
                command.args(["-i".to_owned(), identity.to_owned()]);
            }
            if destination != host {
                command.args(["-o".to_owned(), format!("HostName={destination}")]);
            }
            let remote_command = format!("sh -lc {}", posix_quote(REMOTE_SCRIPT));
            command.arg("--").arg(host).arg(remote_command);
            (target, command)
        }
        _ => anyhow::bail!("select exactly one of --wsl-distro or --ssh-host"),
    };

    #[cfg(windows)]
    command.creation_flags(windows_sys::Win32::System::Threading::CREATE_NO_WINDOW);

    let mut child = match command
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .kill_on_drop(true)
        .spawn()
    {
        Ok(child) => child,
        Err(_) => {
            print_result(result(
                target,
                Status::TransportError,
                Some("probe process could not be started"),
                None,
            ));
            return Ok(());
        }
    };
    let stdout = child
        .stdout
        .take()
        .context("probe stdout was not captured")?;
    let stderr = child
        .stderr
        .take()
        .context("probe stderr was not captured")?;
    let completed = tokio::time::timeout(PROBE_TIMEOUT, async {
        tokio::try_join!(
            child.wait(),
            capture_bounded(stdout),
            capture_bounded(stderr)
        )
    })
    .await;
    let (status, stdout, stderr) = match completed {
        Ok(Ok((status, stdout, stderr))) => (status, stdout, stderr),
        Ok(Err(_)) => {
            cleanup_child(&mut child).await;
            print_result(result(
                target,
                Status::IoError,
                Some("probe process or output stream failed"),
                None,
            ));
            return Ok(());
        }
        Err(_) => {
            cleanup_child(&mut child).await;
            print_result(result(
                target,
                Status::Timeout,
                Some("probe timed out"),
                None,
            ));
            return Ok(());
        }
    };
    if stdout.overflow || stderr.overflow {
        print_result(result(
            target,
            Status::InvalidResponse,
            Some("probe output exceeded the size limit"),
            None,
        ));
        return Ok(());
    }
    if !status.success() {
        let error = if matches!(&target, Target::Ssh { .. }) && is_auth_failure(&stderr.bytes) {
            Status::AuthError
        } else if is_unsupported_shell(&stderr.bytes) {
            Status::Unsupported
        } else {
            Status::TransportError
        };
        let message = match error {
            Status::AuthError => "SSH authentication failed",
            Status::Unsupported => "target shell cannot run the probe",
            _ => "target command failed",
        };
        print_result(result(target, error, Some(message), None));
        return Ok(());
    }
    match parse_output(&stdout.bytes) {
        Some(agents) => print_result(result(target, Status::Ok, None, Some(agents))),
        None => print_result(result(
            target,
            Status::InvalidResponse,
            Some("target returned no valid probe record"),
            None,
        )),
    }
    Ok(())
}

fn validate_value<'a>(flag: &str, value: &'a str) -> Result<&'a str> {
    anyhow::ensure!(
        !value.is_empty() && !value.starts_with('-') && !value.chars().any(char::is_control),
        "invalid value for {flag}"
    );
    Ok(value)
}

fn validate_jump_alias<'a>(alias: &'a str, host: &str) -> Result<&'a str> {
    anyhow::ensure!(
        !alias.is_empty()
            && alias.len() <= 120
            && !alias.starts_with('-')
            && !alias.eq_ignore_ascii_case(host)
            && alias.bytes().any(|byte| byte != b'.')
            && alias
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || b"._-".contains(&byte)),
        "invalid value for --ssh-jump-alias"
    );
    Ok(alias)
}

#[cfg(windows)]
fn trusted_wsl_path() -> Option<PathBuf> {
    std::env::var_os("SystemRoot")
        .map(PathBuf::from)
        .map(|root| root.join("System32").join("wsl.exe"))
        .filter(|path| path.is_file())
}

#[cfg(not(windows))]
fn trusted_wsl_path() -> Option<std::path::PathBuf> {
    None
}

#[cfg(windows)]
fn trusted_ssh_path() -> Option<PathBuf> {
    open_ssh_candidates(
        std::env::var_os("SystemRoot").map(PathBuf::from),
        std::env::var_os("ProgramFiles").map(PathBuf::from),
        std::env::var_os("ProgramFiles(x86)").map(PathBuf::from),
    )
    .into_iter()
    .find(|path| path.is_file())
}

#[cfg(not(windows))]
fn trusted_ssh_path() -> Option<std::path::PathBuf> {
    None
}

fn open_ssh_candidates(
    system_root: Option<std::path::PathBuf>,
    program_files: Option<std::path::PathBuf>,
    program_files_x86: Option<std::path::PathBuf>,
) -> Vec<std::path::PathBuf> {
    [
        system_root.map(|root| root.join("System32").join("OpenSSH").join("ssh.exe")),
        program_files.map(|root| root.join("OpenSSH").join("ssh.exe")),
        program_files_x86.map(|root| root.join("OpenSSH").join("ssh.exe")),
    ]
    .into_iter()
    .flatten()
    .collect()
}

fn posix_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}

async fn cleanup_child(child: &mut tokio::process::Child) {
    let _ = child.start_kill();
    let _ = tokio::time::timeout(CLEANUP_TIMEOUT, child.wait()).await;
}

async fn capture_bounded<R: tokio::io::AsyncRead + Unpin>(
    mut reader: R,
) -> std::io::Result<Captured> {
    let mut bytes = Vec::with_capacity(MAX_STREAM_BYTES);
    let mut overflow = false;
    let mut buffer = [0_u8; 1024];
    loop {
        let read = reader.read(&mut buffer).await?;
        if read == 0 {
            break;
        }
        let remaining = MAX_STREAM_BYTES.saturating_sub(bytes.len());
        let keep = remaining.min(read);
        bytes.extend_from_slice(&buffer[..keep]);
        overflow |= keep < read;
    }
    Ok(Captured { bytes, overflow })
}

fn parse_output(output: &[u8]) -> Option<RemoteAgents> {
    let text = std::str::from_utf8(output).ok()?;
    let mut records = text.lines().filter_map(|line| line.strip_prefix(SENTINEL));
    let payload = records.next()?;
    if records.next().is_some() {
        return None;
    }
    serde_json::from_str(payload).ok()
}

fn is_auth_failure(stderr: &[u8]) -> bool {
    let stderr = String::from_utf8_lossy(stderr).to_ascii_lowercase();
    [
        "permission denied",
        "authentication failed",
        "too many authentication failures",
    ]
    .into_iter()
    .any(|needle| stderr.contains(needle))
}

fn is_unsupported_shell(stderr: &[u8]) -> bool {
    let stderr = String::from_utf8_lossy(stderr).to_ascii_lowercase();
    [
        "command not found",
        "not recognized as an internal",
        "syntax error",
        "bad interpreter",
    ]
    .into_iter()
    .any(|needle| stderr.contains(needle))
}

fn print_result(result: ProbeResult) {
    match serde_json::to_string(&result) {
        Ok(json) => println!("{json}"),
        Err(_) => println!(
            r#"{{"schemaVersion":1,"status":"invalid_response","agents":{{"codex":false,"claude":false,"hermes":false,"qwen":false}},"error":"could not serialize result"}}"#
        ),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_one_valid_sentinel_record_amid_shell_noise() {
        let output = concat!(
            "login banner\n",
            r#"WTA_NATIVE_AGENT_PROBE_V1:{"codex":true,"claude":false,"#,
            r#""hermes":true,"qwen":false}"#,
            "\n"
        )
        .as_bytes();
        let parsed = parse_output(output).unwrap();
        assert!(parsed.codex && parsed.hermes);
        assert!(!parsed.claude && !parsed.qwen);
    }

    #[test]
    fn rejects_missing_duplicate_or_malformed_records() {
        assert!(parse_output(b"noise only\n").is_none());
        assert!(parse_output(b"WTA_NATIVE_AGENT_PROBE_V1:{\"codex\":true}\n").is_none());
        assert!(
            parse_output(b"WTA_NATIVE_AGENT_PROBE_V1:{}\nWTA_NATIVE_AGENT_PROBE_V1:{}\n").is_none()
        );
        assert!(parse_output(b"WTA_NATIVE_AGENT_PROBE_V1:not-json\n").is_none());
    }

    #[test]
    fn auth_failure_classification_does_not_depend_on_exit_code() {
        assert!(is_auth_failure(b"Permission denied (publickey)."));
        assert!(!is_auth_failure(b"Connection timed out"));
    }

    #[test]
    fn clap_requires_exactly_one_target_and_rejects_untyped_ssh_flags() {
        use crate::cli::args::{Cli, Command};
        use clap::Parser;

        assert!(Cli::try_parse_from(["wta", "probe-native-agents"]).is_err());
        assert!(Cli::try_parse_from([
            "wta",
            "probe-native-agents",
            "--wsl-distro",
            "Ubuntu",
            "--ssh-host",
            "box"
        ])
        .is_err());
        assert!(Cli::try_parse_from([
            "wta",
            "probe-native-agents",
            "--ssh-host",
            "box",
            "--ssh-port",
            "22",
            "--ssh-user",
            "u"
        ])
        .is_ok());
        assert!(Cli::try_parse_from([
            "wta",
            "probe-native-agents",
            "--ssh-host",
            "box",
            "--ssh-port",
            "bad"
        ])
        .is_err());
        assert!(matches!(
            Cli::try_parse_from(["wta", "probe-native-agents", "--wsl-distro", "Ubuntu"])
                .unwrap()
                .command,
            Some(Command::ProbeNativeAgents {
                wsl_distro: Some(_),
                ssh_host: None,
                ..
            })
        ));
        assert!(Cli::try_parse_from([
            "wta",
            "probe-native-agents",
            "--ssh-host",
            "box",
            "--",
            "-oProxyCommand=x"
        ])
        .is_err());
    }

    #[test]
    fn validates_jump_aliases_before_using_ssh() {
        assert_eq!(validate_jump_alias("jump-1", "target").unwrap(), "jump-1");
        for invalid in [
            "",
            "-bad",
            "bad host",
            "jump,user",
            "jump:2222",
            "a@b",
            "...",
            "TARGET",
        ] {
            assert!(validate_jump_alias(invalid, "target").is_err(), "{invalid}");
        }
    }

    #[test]
    fn script_checks_all_four_and_rejects_windows_executables() {
        for name in ["codex", "claude", "hermes", "qwen"] {
            assert!(REMOTE_SCRIPT.contains(name));
        }
        assert!(REMOTE_SCRIPT.contains("*.[eE][xX][eE]"));
    }

    #[test]
    fn remote_shell_command_is_posix_quoted_as_one_command() {
        assert_eq!(posix_quote("echo 'hello'"), r#"'echo '\''hello'\'''"#);
        let remote_command = format!("sh -lc {}", posix_quote(REMOTE_SCRIPT));
        assert!(remote_command.starts_with("sh -lc '"));
        assert!(remote_command.ends_with("'"));
    }

    #[test]
    fn json_schema_keeps_target_status_and_all_agent_values() {
        let value = serde_json::to_value(result(
            Target::Ssh {
                host: "build".to_owned(),
            },
            Status::IoError,
            Some("probe process or output stream failed"),
            None,
        ))
        .unwrap();
        assert_eq!(
            value,
            serde_json::json!({
                "schemaVersion": 1,
                "target": {"kind": "ssh", "host": "build"},
                "status": "io_error",
                "agents": {
                    "codex": false,
                    "claude": false,
                    "hermes": false,
                    "qwen": false
                },
                "error": "probe process or output stream failed"
            })
        );
    }

    #[test]
    fn open_ssh_candidates_match_the_windows_resolver_order() {
        let candidates = open_ssh_candidates(
            Some("C:\\Windows".into()),
            Some("C:\\Program Files".into()),
            Some("C:\\Program Files (x86)".into()),
        );
        assert_eq!(
            candidates,
            vec![
                std::path::PathBuf::from(r"C:\Windows\System32\OpenSSH\ssh.exe"),
                std::path::PathBuf::from(r"C:\Program Files\OpenSSH\ssh.exe"),
                std::path::PathBuf::from(r"C:\Program Files (x86)\OpenSSH\ssh.exe"),
            ]
        );
    }
}
