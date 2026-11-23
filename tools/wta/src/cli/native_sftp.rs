//! Bounded SFTP directory listing and downloads over the trusted Windows OpenSSH client.

use anyhow::{Context, Result};
use futures::StreamExt;
use openssh_sftp_client::{error::SftpErrorKind, file::TokioCompatFile, Sftp, SftpOptions};
use serde::Serialize;
use std::{path::Path, process::Stdio, time::Duration};
use tokio::{
    fs::OpenOptions,
    io::{AsyncReadExt, AsyncWriteExt},
    process::Command,
};

const LIST_TIMEOUT: Duration = Duration::from_secs(20);
const DOWNLOAD_TIMEOUT: Duration = Duration::from_secs(6 * 60 * 60);
const CLEANUP_TIMEOUT: Duration = Duration::from_secs(2);
const MAX_ENTRIES: usize = 200;
const MAX_NAME_BYTES: usize = 255;
const MAX_STDERR_BYTES: usize = 8 * 1024;

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "snake_case")]
enum Status {
    Ok,
    Unsupported,
    AuthError,
    TransportError,
    Timeout,
    PermissionDenied,
}

#[derive(Debug, Serialize)]
struct Entry {
    name: String,
    directory: bool,
    size: Option<u64>,
}

#[derive(Debug, Serialize)]
struct ListingResult {
    status: Status,
    #[serde(skip_serializing_if = "Option::is_none")]
    path: Option<String>,
    entries: Vec<Entry>,
    truncated: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<&'static str>,
}

#[derive(Debug, Serialize)]
struct DownloadResult {
    status: Status,
    #[serde(skip_serializing_if = "Option::is_none")]
    bytes: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<&'static str>,
}

impl DownloadResult {
    fn failure(status: Status, error: &'static str) -> Self {
        Self {
            status,
            bytes: None,
            error: Some(error),
        }
    }
}

impl ListingResult {
    fn failure(status: Status, error: &'static str) -> Self {
        Self {
            status,
            path: None,
            entries: Vec::new(),
            truncated: false,
            error: Some(error),
        }
    }
}

pub(crate) async fn run(
    ssh_host: &str,
    ssh_hostname: Option<&str>,
    ssh_port: Option<u16>,
    ssh_user: Option<&str>,
    ssh_identity_file: Option<&str>,
    ssh_jump_alias: Option<&str>,
    path: &str,
) -> Result<()> {
    use super::native_probe::{
        configure_ssh_command, trusted_ssh_path, validate_jump_alias, validate_value,
    };

    let host = validate_value("--ssh-host", ssh_host)?;
    let destination = ssh_hostname
        .map(|value| validate_value("--ssh-hostname", value))
        .transpose()?
        .unwrap_or(host);
    let jump_alias = ssh_jump_alias
        .map(|alias| validate_jump_alias(alias, host))
        .transpose()?;
    let ssh = match trusted_ssh_path() {
        Some(path) => path,
        None => {
            print_result(ListingResult::failure(
                Status::Unsupported,
                "trusted OpenSSH executable is unavailable",
            ));
            return Ok(());
        }
    };

    let mut command = Command::new(ssh);
    configure_ssh_command(
        &mut command,
        host,
        destination,
        ssh_port,
        ssh_user,
        ssh_identity_file,
        jump_alias,
    )?;
    command.arg("-s");
    command.arg("--").arg(host).arg("sftp");

    #[cfg(windows)]
    command.creation_flags(windows_sys::Win32::System::Threading::CREATE_NO_WINDOW);

    let mut child = match command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .spawn()
    {
        Ok(child) => child,
        Err(_) => {
            print_result(ListingResult::failure(
                Status::TransportError,
                "OpenSSH process could not be started",
            ));
            return Ok(());
        }
    };
    let stdin = child.stdin.take().context("SFTP stdin was not captured")?;
    let stdout = child
        .stdout
        .take()
        .context("SFTP stdout was not captured")?;
    let stderr = child.stderr.take().context("SSH stderr was not captured")?;
    let stderr_task = tokio::spawn(capture_bounded(stderr));

    let operation = tokio::time::timeout(LIST_TIMEOUT, async {
        let sftp = Sftp::new(stdin, stdout, SftpOptions::default()).await?;
        let mut remote_fs = sftp.fs();
        let canonical = remote_fs.canonicalize(path).await?;
        let canonical = canonical
            .to_str()
            .context("canonical SFTP path is not valid Unicode")?
            .to_owned();
        anyhow::ensure!(
            !canonical.is_empty()
                && canonical.len() <= 8192
                && !canonical.chars().any(char::is_control),
            "canonical SFTP path is invalid"
        );
        let dir = remote_fs.open_dir(&canonical).await?;
        let mut reader = Box::pin(dir.read_dir());
        let mut entries = Vec::with_capacity(MAX_ENTRIES);
        let mut truncated = false;
        while let Some(entry) = reader.next().await {
            let entry = entry?;
            let name_path = entry.filename();
            let Some(name) = name_path.to_str() else {
                continue;
            };
            if !safe_name(name) {
                continue;
            }
            if entries.len() == MAX_ENTRIES {
                truncated = true;
                break;
            }
            let metadata = entry.metadata();
            entries.push(Entry {
                name: name.to_owned(),
                directory: metadata
                    .file_type()
                    .is_some_and(|file_type| file_type.is_dir()),
                size: metadata.len(),
            });
        }
        drop(reader);
        drop(remote_fs);
        sftp.close().await?;
        Ok::<_, anyhow::Error>(ListingResult {
            status: Status::Ok,
            path: Some(canonical),
            entries,
            truncated,
            error: None,
        })
    })
    .await;

    match operation {
        Ok(Ok(listing)) => {
            if tokio::time::timeout(CLEANUP_TIMEOUT, child.wait())
                .await
                .is_err()
            {
                cleanup_child(&mut child).await;
            }
            let _ = tokio::time::timeout(CLEANUP_TIMEOUT, stderr_task).await;
            print_result(listing);
        }
        Ok(Err(error)) => {
            cleanup_child(&mut child).await;
            let stderr = collect_stderr(stderr_task).await;
            print_result(classify_error(&error, &stderr));
        }
        Err(_) => {
            cleanup_child(&mut child).await;
            let stderr = collect_stderr(stderr_task).await;
            let result = if is_auth_failure(&stderr) {
                ListingResult::failure(Status::AuthError, "SSH authentication failed")
            } else {
                ListingResult::failure(Status::Timeout, "SFTP request timed out")
            };
            print_result(result);
        }
    }
    Ok(())
}

pub(crate) async fn download(
    ssh_host: &str,
    ssh_hostname: Option<&str>,
    ssh_port: Option<u16>,
    ssh_user: Option<&str>,
    ssh_identity_file: Option<&str>,
    ssh_jump_alias: Option<&str>,
    remote_path: &str,
    local_path: &Path,
) -> Result<()> {
    use super::native_probe::{
        configure_ssh_command, trusted_ssh_path, validate_jump_alias, validate_value,
    };

    let host = validate_value("--ssh-host", ssh_host)?;
    let destination = ssh_hostname
        .map(|value| validate_value("--ssh-hostname", value))
        .transpose()?
        .unwrap_or(host);
    let jump_alias = ssh_jump_alias
        .map(|alias| validate_jump_alias(alias, host))
        .transpose()?;
    if remote_path.is_empty()
        || remote_path.len() > 8192
        || remote_path.chars().any(char::is_control)
    {
        print_download_result(DownloadResult::failure(
            Status::TransportError,
            "remote file path is invalid",
        ));
        return Ok(());
    }
    let ssh = match trusted_ssh_path() {
        Some(path) => path,
        None => {
            print_download_result(DownloadResult::failure(
                Status::Unsupported,
                "trusted OpenSSH executable is unavailable",
            ));
            return Ok(());
        }
    };
    let mut command = Command::new(ssh);
    configure_ssh_command(
        &mut command,
        host,
        destination,
        ssh_port,
        ssh_user,
        ssh_identity_file,
        jump_alias,
    )?;
    command.arg("-s").arg("--").arg(host).arg("sftp");
    #[cfg(windows)]
    command.creation_flags(windows_sys::Win32::System::Threading::CREATE_NO_WINDOW);

    let mut local = match OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(local_path)
        .await
    {
        Ok(file) => file,
        Err(_) => {
            print_download_result(DownloadResult::failure(
                Status::TransportError,
                "temporary download file could not be created",
            ));
            return Ok(());
        }
    };

    let mut child = match command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .spawn()
    {
        Ok(child) => child,
        Err(_) => {
            drop(local);
            remove_temp_file(local_path).await;
            print_download_result(DownloadResult::failure(
                Status::TransportError,
                "OpenSSH process could not be started",
            ));
            return Ok(());
        }
    };
    let stdin = child.stdin.take().context("SFTP stdin was not captured")?;
    let stdout = child
        .stdout
        .take()
        .context("SFTP stdout was not captured")?;
    let stderr = child.stderr.take().context("SSH stderr was not captured")?;
    let stderr_task = tokio::spawn(capture_bounded(stderr));

    let connect = tokio::time::timeout(
        LIST_TIMEOUT,
        Sftp::new(stdin, stdout, SftpOptions::default()),
    )
    .await;
    let sftp = match connect {
        Ok(Ok(sftp)) => sftp,
        Ok(Err(error)) => {
            cleanup_child(&mut child).await;
            let stderr = collect_stderr(stderr_task).await;
            drop(local);
            remove_temp_file(local_path).await;
            print_download_result(classify_download_error(&anyhow::Error::new(error), &stderr));
            return Ok(());
        }
        Err(_) => {
            cleanup_child(&mut child).await;
            let stderr = collect_stderr(stderr_task).await;
            drop(local);
            remove_temp_file(local_path).await;
            let failure = if is_auth_failure(&stderr) {
                DownloadResult::failure(Status::AuthError, "SSH authentication failed")
            } else {
                DownloadResult::failure(Status::Timeout, "SFTP connection timed out")
            };
            print_download_result(failure);
            return Ok(());
        }
    };

    let transfer = tokio::time::timeout(DOWNLOAD_TIMEOUT, async {
        let remote_file = sftp.options().read(true).open(remote_path).await?;
        let close_file = remote_file.clone();
        let mut reader = Box::pin(TokioCompatFile::from(remote_file));
        let mut pinned_reader = reader.as_mut();
        let bytes = tokio::io::copy(&mut pinned_reader, &mut local).await?;
        local.flush().await?;
        drop(reader);
        close_file.close().await?;
        sftp.close().await?;
        Ok::<_, anyhow::Error>(bytes)
    })
    .await;

    match transfer {
        Ok(Ok(bytes)) => {
            drop(local);
            if tokio::time::timeout(CLEANUP_TIMEOUT, child.wait())
                .await
                .is_err()
            {
                cleanup_child(&mut child).await;
            }
            let _ = tokio::time::timeout(CLEANUP_TIMEOUT, stderr_task).await;
            print_download_result(DownloadResult {
                status: Status::Ok,
                bytes: Some(bytes),
                error: None,
            });
        }
        Ok(Err(error)) => {
            cleanup_child(&mut child).await;
            let stderr = collect_stderr(stderr_task).await;
            drop(local);
            remove_temp_file(local_path).await;
            print_download_result(classify_download_error(&error, &stderr));
        }
        Err(_) => {
            cleanup_child(&mut child).await;
            let stderr = collect_stderr(stderr_task).await;
            drop(local);
            remove_temp_file(local_path).await;
            let failure = if is_auth_failure(&stderr) {
                DownloadResult::failure(Status::AuthError, "SSH authentication failed")
            } else {
                DownloadResult::failure(Status::Timeout, "SFTP download timed out")
            };
            print_download_result(failure);
        }
    }
    Ok(())
}

fn safe_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= MAX_NAME_BYTES
        && name != "."
        && name != ".."
        && !name.contains('/')
        && !name.contains('\\')
        && !name.chars().any(char::is_control)
}

fn classify_error(error: &anyhow::Error, stderr: &[u8]) -> ListingResult {
    if is_auth_failure(stderr) {
        return ListingResult::failure(Status::AuthError, "SSH authentication failed");
    }
    for cause in error.chain() {
        if let Some(sftp_error) = cause.downcast_ref::<openssh_sftp_client::Error>() {
            if matches!(
                sftp_error,
                openssh_sftp_client::Error::SftpError(SftpErrorKind::PermDenied, _)
            ) {
                return ListingResult::failure(
                    Status::PermissionDenied,
                    "remote path access was denied",
                );
            }
            if matches!(
                sftp_error,
                openssh_sftp_client::Error::UnsupportedSftpProtocol { .. }
                    | openssh_sftp_client::Error::SftpServerFailure(_)
            ) {
                return ListingResult::failure(
                    Status::Unsupported,
                    "remote SFTP subsystem is unavailable",
                );
            }
        }
    }
    if is_unsupported_subsystem(stderr) {
        return ListingResult::failure(Status::Unsupported, "remote SFTP subsystem is unavailable");
    }
    ListingResult::failure(Status::TransportError, "SFTP request failed")
}

fn classify_download_error(error: &anyhow::Error, stderr: &[u8]) -> DownloadResult {
    let listing_result = classify_error(error, stderr);
    DownloadResult::failure(
        listing_result.status,
        listing_result.error.unwrap_or("SFTP request failed"),
    )
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

fn is_unsupported_subsystem(stderr: &[u8]) -> bool {
    let stderr = String::from_utf8_lossy(stderr).to_ascii_lowercase();
    [
        "subsystem request failed",
        "subsystem not found",
        "unknown subsystem",
    ]
    .into_iter()
    .any(|needle| stderr.contains(needle))
}

async fn capture_bounded<R: tokio::io::AsyncRead + Unpin>(
    mut reader: R,
) -> std::io::Result<Vec<u8>> {
    let mut bytes = Vec::with_capacity(MAX_STDERR_BYTES);
    let mut buffer = [0_u8; 1024];
    loop {
        let read = reader.read(&mut buffer).await?;
        if read == 0 {
            break;
        }
        let remaining = MAX_STDERR_BYTES.saturating_sub(bytes.len());
        bytes.extend_from_slice(&buffer[..remaining.min(read)]);
    }
    Ok(bytes)
}

async fn collect_stderr(task: tokio::task::JoinHandle<std::io::Result<Vec<u8>>>) -> Vec<u8> {
    tokio::time::timeout(CLEANUP_TIMEOUT, task)
        .await
        .ok()
        .and_then(Result::ok)
        .and_then(Result::ok)
        .unwrap_or_default()
}

async fn cleanup_child(child: &mut tokio::process::Child) {
    let _ = child.start_kill();
    let _ = tokio::time::timeout(CLEANUP_TIMEOUT, child.wait()).await;
}

async fn remove_temp_file(path: &Path) {
    let _ = tokio::fs::remove_file(path).await;
}

fn print_result(result: ListingResult) {
    match serde_json::to_string(&result) {
        Ok(json) => println!("{json}"),
        Err(_) => println!(
            "{}",
            r#"{"status":"transport_error","entries":[],"truncated":false,"error":"could not serialize result"}"#
        ),
    }
}

fn print_download_result(result: DownloadResult) {
    match serde_json::to_string(&result) {
        Ok(json) => println!("{json}"),
        Err(_) => println!(
            "{}",
            r#"{"status":"transport_error","error":"could not serialize result"}"#
        ),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn listing_names_are_bounded_single_components() {
        assert!(safe_name("workspace"));
        assert!(safe_name("файл.rs"));
        for name in ["", ".", "..", "a/b", "a\\b", "bad\nname"] {
            assert!(!safe_name(name), "{name:?}");
        }
        assert!(!safe_name(&"x".repeat(MAX_NAME_BYTES + 1)));
    }

    #[test]
    fn list_arguments_require_host_and_path_and_accept_explicit_identity() {
        use crate::cli::args::{Cli, Command};
        use clap::Parser;

        assert!(Cli::try_parse_from(["wta", "sftp-list"]).is_err());
        assert!(matches!(
            Cli::try_parse_from([
                "wta",
                "sftp-list",
                "--ssh-host",
                "prod",
                "--ssh-identity-file",
                "C:\\keys\\id_ed25519",
                "--path=/srv/work"
            ])
            .unwrap()
            .command,
            Some(Command::SftpList { .. })
        ));
    }

    #[test]
    fn download_arguments_require_a_separate_local_path() {
        use crate::cli::args::{Cli, Command};
        use clap::Parser;

        assert!(Cli::try_parse_from([
            "wta",
            "sftp-download",
            "--ssh-host",
            "prod",
            "--path=/srv/file",
        ])
        .is_err());
        assert!(matches!(
            Cli::try_parse_from([
                "wta",
                "sftp-download",
                "--ssh-host",
                "prod",
                "--path=/srv/file",
                "--local-path",
                "C:\\Temp\\download-123"
            ])
            .unwrap()
            .command,
            Some(Command::SftpDownload { .. })
        ));
    }

    #[test]
    fn download_success_schema_reports_only_status_and_byte_count() {
        let json = serde_json::to_value(DownloadResult {
            status: Status::Ok,
            bytes: Some(123),
            error: None,
        })
        .unwrap();
        assert_eq!(json, serde_json::json!({"status":"ok", "bytes":123}));
    }

    #[test]
    fn output_uses_stable_field_names_and_sanitized_optional_error() {
        let json = serde_json::to_value(ListingResult {
            status: Status::Ok,
            path: Some("/srv".to_owned()),
            entries: vec![Entry {
                name: "project".to_owned(),
                directory: true,
                size: None,
            }],
            truncated: false,
            error: None,
        })
        .unwrap();
        assert_eq!(
            json,
            serde_json::json!({
                "status": "ok",
                "path": "/srv",
                "entries": [{"name":"project", "directory":true, "size":null}],
                "truncated": false
            })
        );
    }
}
