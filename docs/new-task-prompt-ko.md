# Codex 새 작업 시작 프롬프트

새 task에서 로컬 저장소 절대 경로를 직접 고정하지 않는다. workspace 루트의
`AGENTS.md`와 `agent-rules/active-worktree-ko.md`가 현재 활성 저장소를 결정한다.

## RPi VMS task

```text
workspace 루트의 AGENTS.md와 agent-rules/active-worktree-ko.md,
agent-rules/workflow-ko.md를 순서대로 읽어라.

active-worktree-ko.md가 지정한 저장소에서 git status와 최근 commit을 확인하고,
그 저장소의 AGENTS.md, docs/current-status-ko.md,
docs/vms-development-roadmap-ko.md를 읽어라.

이미 실제 Raspberry Pi에서 통과한 작업을 다시 만들지 말고 현재 milestone에서
이어가라. 이번 작업은 [작업 내용을 한 문장으로 작성]이다.

다른 task의 변경을 건드리지 말고, 의미 있는 구현·검증 후 current-status-ko.md를
사실에 맞게 갱신하라. 수정 파일, Pi 반영 여부, build/test 명령과 시간, 정상 출력,
실패 로그, 미검증 항목을 인계하라.
```

권장 task 이름:

```text
[RPi-VMS M1-B] 영상 end-to-end 200ms baseline 계측
[RPi-VMS M2] 단일 채널 ChannelIngest
[RPi-VMS M3] shared live RTSP/RTSPS server
[RPi-VMS M4] 4채널 ChannelManager 및 부하 시험
[RPi-VMS M6] systemd, mount dependency, journald
```

## Qt Viewer task

```text
workspace 루트의 AGENTS.md와 agent-rules/workflow-ko.md를 읽어라.
Qt source는 C:\Users\shini\Documents\QtProjects\qt_4ch_viewer다.

agent-rules/active-worktree-ko.md가 지정한 RPi VMS 저장소의
docs/current-status-ko.md와 docs/latency-measurement-ko.md를 읽어라.

이번 작업은 [Qt 작업 내용을 한 문장으로 작성]이다. 현재 Qt delay가 영상
end-to-end가 아니라 RGB frame 생성 이후 GUI queue 지연만 나타낸다는 점을
전제로 기존 FFmpeg/Qt thread 구조 안에서 작업하라.
```

## task 분리 기준

- 같은 milestone의 구현, 오류 수정, Pi 재시험은 같은 task에서 이어간다.
- RPi와 Qt처럼 code ownership이 다르면 task를 분리한다.
- 단순 질문마다 새 task를 만들지 않는다.
- task 종료 전 다음 작업에 필요한 실제 결과를 status 문서에 남긴다.
