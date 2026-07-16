# RPi VMS Repository Instructions

- Default communication language is Korean.
- Read `docs/current-status-ko.md` and `docs/vms-development-roadmap-ko.md` before implementation.
- When present, read the ignored `docs/current-environment-local-ko.md` for deployment-specific hardware and network details.
- Camera channels are `0..3`; VMS, DB, client, and storage channel IDs are `1..4`.
- Store DB, log, and protocol timestamps in UTC. Use a monotonic clock for latency inside one device.
- Keep camera ingest independent from client sessions. Do not transcode recording or live streams by default.
- Do not hard-code camera passwords, TLS private keys, or authentication tokens.
- Require the ext4 storage mount for production recording. Never silently fall back to the OS SD card.
- Reuse the existing RTSP parser, Digest, TLS, and logging code when implementing VMS networking.
- Apply the repository `.clang-format` only to modified C/C++ files, except in an explicit formatting-only commit.
- Treat Raspberry Pi camera, USB, and network checks as hardware integration tests; x86 CI does not replace them.
- After edits, report changed files, deployment steps, exact build/test commands, expected output, failure logs, and unverified items.
- End every completed task with the current state, exactly one recommended next task, and who acts next.
- Do not stop at "no further user testing is required." When roadmap work remains, select the next item and state whether the agent continues now or the user must open a new task.
- If a new task is required, provide its exact title and starter message. If the user has nothing to do, state `사용자 행동 없음` and name the next agent action.
