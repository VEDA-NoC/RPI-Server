# RPi VMS 저장소 보충 규칙

workspace 안에서 작업할 때는 루트 `AGENTS.md`, `agent-rules/active-worktree-ko.md`,
`agent-rules/workflow-ko.md`를 먼저 적용한다. 이 파일은 저장소에만 필요한 규칙을
보충하며 공통 규칙을 반복하지 않는다.

## 시작 시 읽을 문서

1. `docs/current-status-ko.md`
2. `docs/vms-development-roadmap-ko.md`
3. 현재 작업과 직접 관련된 설계 문서 하나
4. 배치 정보가 필요하면 Git에서 제외된 `docs/current-environment-local-ko.md`

## 저장소 설계 기준

- camera 채널은 `0..3`, VMS·DB·client·storage 채널은 `1..4`다.
- DB, 로그, protocol timestamp는 UTC로 저장하고 같은 장치 내부 지연은 monotonic
  clock으로 측정한다.
- camera ingest는 client session과 독립적으로 유지한다.
- recording/live 경로는 기본적으로 transcoding하지 않는다.
- 운영 녹화는 ext4 외장 storage mount를 요구하며 SD카드로 우회하지 않는다.
- VMS networking은 기존 RTSP parser, Digest, TLS, logging 코드를 재사용한다.
- 실제 camera, Raspberry Pi, USB, network 검증은 x86 CI로 대체하지 않는다.

## 보안과 형식

- camera password, TLS private key, token을 source·문서·unit에 hard-code하지 않는다.
- C/C++은 가장 가까운 `.clang-format`을 사용하고 수정 파일만 format한다.
- feature PR은 필요한 구현·문서·자동·장치 검증이 끝날 때까지 draft로 둔다.
