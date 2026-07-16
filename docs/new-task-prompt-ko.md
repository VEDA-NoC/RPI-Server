# Codex 새 작업 시작 프롬프트

## RPi VMS 프로젝트용

```text
이 프로젝트의 AGENTS.md를 먼저 읽어라.
그 다음 outputs/rpi-vms/docs/current-status-ko.md,
outputs/rpi-vms/docs/vms-development-roadmap-ko.md를 순서대로 읽어라.

이미 실제 Raspberry Pi에서 통과한 작업을 다시 만들지 말고 현재 milestone에서
이어가라. 이번 작업은 [작업 내용을 한 문장으로 작성]이다.

실제 Git 저장소 루트는 outputs/rpi-vms다. 이미 Git 초기화, baseline commit,
clang-format 19 전체 적용, GitHub Actions 추가가 끝났으므로 다시 수행하지 마라.
작업 시작 시 git status와 최근 commit을 확인하고 feature branch에서 작업하라.
GitHub 원격은 VEDA-NoC/RPI-Server이며 current-status-ko.md에 push/CI 결과가
기록되어 있지 않으면 먼저 그 상태를 확인하라.

기존 rpi-vms 구조와 기존 RTSPS proxy 코드를 먼저 조사하고 재사용하라.
파일을 수정하면 반드시 다음을 마지막 응답에 포함하라.
- 수정 파일 목록
- Pi에 복사해야 하는 파일
- 빌드 명령
- 테스트 명령과 필요한 실행 시간
- 정상 예상 출력
- 실패 시 수집할 로그
- 직접 실행하지 못한 검증

의미 있는 구현/검증이 끝나면 current-status-ko.md도 사실에 맞게 갱신하라.
```

예시 작업 이름:

```text
[RPi-VMS M1] 시간 동기화 및 GStreamer 지연 계측
[RPi-VMS M2] 단일 채널 ChannelIngest
[RPi-VMS M3] shared live RTSP/RTSPS server
[RPi-VMS M4] 4채널 ChannelManager 및 부하 시험
[RPi-VMS M6] systemd, mount dependency, journald
```

## Qt Viewer 프로젝트용

```text
Qt 프로젝트는 C:\Users\shini\Documents\QtProjects\qt_4ch_viewer다.
먼저 다음 VMS 문서를 읽어라.
- C:\Users\shini\Documents\Codex\2026-07-10\rtsps-codex-hanwha-rtsp-raspberry-pi\AGENTS.md
- C:\Users\shini\Documents\Codex\2026-07-10\rtsps-codex-hanwha-rtsp-raspberry-pi\outputs\rpi-vms\docs\current-status-ko.md
- C:\Users\shini\Documents\Codex\2026-07-10\rtsps-codex-hanwha-rtsp-raspberry-pi\outputs\rpi-vms\docs\latency-measurement-ko.md

이번 작업은 [Qt 작업 내용을 한 문장으로 작성]이다.
현재 표시되는 delay는 end-to-end가 아니라 RGB frame 생성 이후 GUI queue
지연뿐이라는 점을 전제로 작업하라. 기존 FFmpeg/Qt thread 구조를 먼저 읽고
기존 동작을 유지하면서 계측을 추가하라.

파일을 수정하면 빌드 명령, 실행 방법, UI/로그의 정상 예상값, 직접 검증하지 못한
항목을 마지막 응답에 포함하라.
```

예시 작업 이름:

```text
[Qt M1] FFmpeg decode 및 GUI queue 지연 계측
[Qt M3] VMS shared live session 연결
[Qt M5] live profile/substream 비교 UI
[Qt M8] 녹화 timeline 및 playback
```

## 대화 분리 규칙

- 같은 milestone의 구현, 오류 수정, Pi 재시험은 같은 task에서 이어간다.
- 코드 소유 폴더가 다르면 RPi task와 Qt task를 분리한다.
- SUNAPI PDF 조사처럼 결과가 문서로 귀결되는 독립 작업은 별도 task로 분리할 수 있다.
- 단순 질문마다 새 task를 만들지 않는다.
- 새 task를 시작할 때는 이전 대화 전체를 붙이지 말고 status와 설계 문서를 읽게 한다.
- task 종료 전에 다음 task가 알아야 할 실제 결과를 status에 반영한다.
