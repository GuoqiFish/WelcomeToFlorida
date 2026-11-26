use anyhow::{Context, Result};
use std::io::{BufRead, BufReader};
use std::os::windows::process::CommandExt;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};
use std::{sync::mpsc, thread};
use windows::Win32::System::Threading::CREATE_NO_WINDOW;

use crate::preload::{Progress, Window};

fn needed(bytes: &[u8]) -> Option<u32> {
    std::str::from_utf8(bytes)
        .ok()?
        .trim()
        .strip_prefix("precompile-check ")?
        .parse()
        .ok()
}

pub(crate) fn before_play(configure: &impl Fn() -> Result<Command>) -> Result<()> {
    if ["VICTORY_PIPELINE_PRECOMPILE", "VICTORY_PIPELINE_WARMUP"]
        .iter()
        .any(|key| std::env::var_os(key).is_some_and(|v| v == "0"))
    {
        return Ok(());
    }
    let mut command = configure()?;
    command.arg("precompile").creation_flags(CREATE_NO_WINDOW.0);
    let check = configure()?
        .args(["precompile", "--check"])
        .creation_flags(CREATE_NO_WINDOW.0)
        .output()?;
    // Older packaged engines do not have this command.
    let Some(total) = check
        .status
        .success()
        .then(|| needed(&check.stdout))
        .flatten()
        .filter(|&v| v > 0)
    else {
        return Ok(());
    };
    let mut window = Window::open()?;
    let child = command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()?;
    follow(child, total, |tick| window.run(tick))
}

type Tick<'a> = dyn FnMut(bool) -> Result<(Progress, bool)> + 'a;

fn follow(
    mut child: Child,
    total: u32,
    show: impl FnOnce(&mut Tick<'_>) -> Result<()>,
) -> Result<()> {
    let mut input = child.stdin.take();
    let output = child
        .stdout
        .take()
        .context("Sortie du moteur indisponible")?;
    let (tx, rx) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(output).lines().map_while(Result::ok) {
            if let Some(value) = Progress::parse(&line) {
                let _ = tx.send(value);
            }
        }
    });
    let mut value = Progress {
        done: 0,
        total,
        failed: 0,
    };
    let mut finished = None;
    let shown = show(&mut |skip| {
        if skip {
            input.take();
        }
        let read_all = reader.is_finished();
        for update in rx.try_iter() {
            value = update;
        }
        if finished.is_none() && read_all && child.try_wait()?.is_some() {
            finished = Some(Instant::now());
        }
        let close = finished.is_some_and(|t| skip || t.elapsed() > Duration::from_millis(380));
        Ok((value, close))
    });
    drop(input);
    let status = child.wait()?;
    let _ = reader.join();
    shown?;
    anyhow::ensure!(status.success(), "Le préchargement a quitté avec {status}.");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn worker_finishes_before_handoff_on_completion_and_skip() -> Result<()> {
        for skip in [false, true] {
            let script = if skip {
                "Write-Output 'precompile 0 2 0'; [Console]::In.ReadToEnd() | Out-Null; Write-Output 'precompile 1 2 0'"
            } else {
                "Write-Output 'precompile 0 2 0'; Write-Output 'precompile 2 2 0'"
            };
            let child = Command::new("powershell.exe")
                .args(["-NoProfile", "-NonInteractive", "-Command", script])
                .creation_flags(CREATE_NO_WINDOW.0)
                .stdin(Stdio::piped())
                .stdout(Stdio::piped())
                .spawn()?;
            let deadline = Instant::now() + Duration::from_secs(20);
            follow(child, 2, |tick| {
                loop {
                    let (value, close) = tick(skip)?;
                    if close {
                        assert_eq!(value.done, if skip { 1 } else { 2 });
                        return Ok(());
                    }
                    anyhow::ensure!(Instant::now() < deadline, "worker did not finish");
                    thread::sleep(Duration::from_millis(10));
                }
            })?;
        }
        Ok(())
    }

    #[test]
    fn check_requires_the_engines_answer() {
        assert_eq!(needed(b"precompile-check 17\r\n"), Some(17));
        assert_eq!(needed(b"precompile-check 0\n"), Some(0));
        assert_eq!(needed(b"unexpected command"), None);
    }
}
