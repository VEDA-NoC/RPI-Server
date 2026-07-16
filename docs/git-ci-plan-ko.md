# Git 및 CI 운영 계획

## 시작 시점

`ChannelIngest`, live fan-out, 4채널 `ChannelManager`처럼 구조 변경이 시작되기
전에 Git 저장소를 만든다. 현재 단일 채널 녹화 성공 상태를 첫 baseline으로 보존한다.

권장 Git 저장소 루트는 독립 제품 폴더인 `outputs/rpi-vms`다. SUNAPI PDF와 `work`
추출 파일은 저장소 밖의 참고 자료로 유지한다.

## 현재 구현 상태

2026-07-16 기준:

- 로컬 Git 저장소 루트: `outputs/rpi-vms`
- baseline commit: `fa6ccad`
- clang-format 19 commit: `5b935ae`
- CI commit: `170727b`
- workflow: `.github/workflows/ci.yml`
- trigger: `pull_request`, `workflow_dispatch`
- runner: `ubuntu-24.04` x86_64
- jobs: format, cppcheck, CMake build 및 CLI smoke test
- Raspberry Pi hardware test와 deploy job: 미구현
- GitHub 원격: `VEDA-NoC/RPI-Server`
- visibility: Public, 배치별 장치 정보와 credential은 Git 추적에서 제외
- PR #1 run #2에서 format, cppcheck, x86_64 build, CLI smoke test 통과

## Formatting

- RPi C++: `outputs/rpi-vms/.clang-format`
- Qt C++: Qt 프로젝트 루트의 별도 `.clang-format`
- 가장 가까운 `.clang-format`이 해당 코드의 규칙이다.
- 완료 전에 수정한 C/C++ 파일에만 `clang-format -i`를 실행한다.
- CI에서는 자동 수정하지 않고 `--dry-run --Werror`로 차이만 검출한다.
- 저장소 전체 일괄 포맷은 별도 formatting-only commit/PR에서만 수행한다.

## Pull Request 흐름

```text
feature branch
  -> local format/build/test
  -> push
  -> PR open
  -> format check
  -> static analysis
  -> x86_64 build
  -> unit/integration tests without camera
  -> team review
  -> merge
  -> approved Raspberry Pi hardware test
```

PR이 workflow를 시작한다. workflow가 PR을 시작하는 구조가 아니다.

## GitHub Actions Jobs

### 1. format

`clang-format --dry-run --Werror`로 검사한다.

### 2. static-analysis

- `clang-tidy`는 CMake `compile_commands.json`을 사용한다.
- 보조 분석으로 `cppcheck`를 사용할 수 있다.
- convention은 `.clang-format`, `.clang-tidy`, compiler warning으로 기계화한다.

### 3. build

- GitHub-hosted Ubuntu x86_64에서 GStreamer/SQLite/OpenSSL dependency를 설치한다.
- `-Wall -Wextra -Wpedantic`과 선택한 warning policy를 적용한다.
- x86_64 build는 일반 C++ 오류를 잡지만 ARM64/Pi 검증을 대체하지 않는다.

### 4. tests

- config/parser/DB/policy/state-machine unit test
- 임시 SQLite DB test
- `videotestsrc` 또는 test RTSP server 기반 GStreamer test
- Hanwha camera, USB mount, Tailscale 시험은 hardware test로 분리한다.

### 5. artifact

- build log, test report, static analysis report를 보존한다.
- x86_64 binary를 Raspberry Pi 배포물로 사용하지 않는다.

## Raspberry Pi Hardware Test

외부 contributor 또는 검토 전 PR에서 self-hosted Pi runner를 실행하지 않는다.
PR 코드는 runner 권한으로 LAN, camera credential, storage에 접근할 수 있기 때문이다.

현재 단계는 production deploy가 아니라 source sync, Pi native build, 실제 camera/USB
integration test다. systemd와 release package가 생기기 전에는 deploy job을 만들지
않는다.

향후 원격 hardware test를 자동화할 때는 다음 중 하나로 제한한다.

```text
workflow_dispatch + GitHub Environment approval
또는
approved merge to main + trusted self-hosted runner
```

초기에는 수동 복사와 Pi native build/test를 유지해도 의미가 있다. CI는 반복 가능한
format/build/unit test 회귀를 잡고, 수동 Pi 시험은 ARM64, camera, USB, network를
검증한다.

## Branch Protection

- main direct push 금지
- format/static-analysis/build/tests required
- 최소 1명 review required
- hardware test는 required PR check가 아닌 승인된 후속 단계로 운영
