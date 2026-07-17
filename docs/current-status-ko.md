# RPi VMS 현재 상태

마지막 갱신: 2026-07-17

## 검증 환경 개요

```text
Raspberry Pi 4 8GB
board=Raspberry Pi 4 Model B Rev 1.5
CPU=Cortex-A72, 4 cores, 600MHz..1800MHz
RAM=7.6GiB
Raspberry Pi OS, user=noc
OS=Debian GNU/Linux 13.5 (trixie)
kernel=6.18.37-v8+ aarch64 PREEMPT
VMS source=~/rpi-vms
camera=Hanwha multi-sensor network camera
test storage=Imation Nano Pro II 16GB USB 2.0
storage filesystem=ext4
mount=/mnt/vms-storage
GStreamer=1.26.2
g++=14.2.0
CMake=3.31.6
SQLite=3.46.1
OpenSSL=3.5.6
```

실제 camera IP, 모델/firmware, storage UUID 같은 배치별 값은 Git에서 제외된
`docs/current-environment-local-ko.md`에 기록한다.

500GB WD Blue HDD/USB enclosure는 USB enumeration `error -71` 문제로 교환 중이다.
16GB USB 메모리는 같은 Pi USB 계통에서 정상 인식됐으므로 Pi USB host 자체는
동작한다.

## 완료 및 실제 검증

### 저장장치

- USB ext4 포맷 성공
- `/dev/sda1` -> `/mnt/vms-storage` mount 성공
- `--require-storage-mount`에서 `mount_point=yes` 확인
- `lost+found`는 ext4 복구용 root 전용 디렉터리이며 정상
- `vcgencmd get_throttled` 결과 `0x0`

자동 mount용 `/etc/fstab` 설정은 아직 최종 확인하지 않았다.

### 채널 번호

```text
camera_channel=0
channel_id=1
RTSP path=/0/profile2/media.smp
storage=/mnt/vms-storage/recordings/ch1
DB recording_segments.channel_id=1
```

### 녹화

2026-07-16 실제 Pi 시험에서 다음을 확인했다.

- H.264 2592x1520, 30 fps 입력
- RTSP over TCP와 Digest 인증 성공
- 10초 기준 MP4 분할 성공
- `ch1_*.mp4` 4개 생성
- SQLite row 4개 생성
- 모든 row `complete=1`
- 파일 크기 약 2.1MB~4.9MB

120초 이전 시험에서는 13개 segment가 정상 생성됐으며 중간 segment duration은
약 9~10초였다. 실행 직후 너무 빨리 종료한 시험에서 생성된 0~1초 파일은 정상
장시간 녹화 판정에서 제외한다.

2026-07-16 Public 저장소 정리 이후 Pi 재시험:

- `make clean && make` ARM64 native build 성공
- camera host 없이 실행할 때 `error: --camera-host is required`, exit code 1 확인
- ext4 mount에서 35초 녹화 및 MP4 segment 2개 종료 처리 성공
- pipeline 로그의 RTSP userinfo가 `rtsp://<redacted>@...`로 마스킹됨
- 파일 크기 약 3.6MB, 1.0MB
- `.github`는 Pi runtime에 필요하지 않으며 sync script에서 제외
- `tools/sync-to-pi.sh --build`로 실제 source 동기화와 ARM64 재빌드 성공
- 원격 확인 결과 `.github` 없음, 실행 가능한 `app` 존재
- `--dry-run --clean`에서 `app`은 보호되고 구형 문서 3개만 삭제 예정으로 표시
- 실제 `--clean` 삭제는 아직 실행하지 않음

### GitHub Actions

2026-07-16 PR #1에서 초기 CI를 검증했다.

- run #1: format/build 성공, cppcheck performance 경고 2건으로 실패
- `trim_copy` 불필요 복사와 test client의 불필요한 `substr` 대입 수정
- run #2와 최종 문서 반영 run #3: clang-format 19, cppcheck, x86_64 CMake build,
  CLI smoke test 모두 성공
- PR #1 merge 완료
- Raspberry Pi, camera, USB storage 시험은 CI에 포함하지 않음

## 시간 동기화 상태

### M1-A read-only 측정 도구

2026-07-16 구현:

- `tools/measure_time_offsets.py` 추가
- Windows Time 상태와 Pi의 기존 `tools/check-time-sync.sh` 출력을 한 번에 수집
- 동일한 지정 NTP 서버에 대해 Windows/Pi가 각각 SNTP offset과 RTT를 측정
- Hanwha SUNAPI date `action=view`만 사용해 현재 시간/NTP 설정 조회
- 카메라 비밀번호는 대화형 입력 후 메모리에서만 사용하며 결과에 저장하지 않음
- Windows/Pi/camera 상대 offset과 추정 불확실성을 JSON으로 기록
- SUNAPI 초 단위 시각 해상도에 따른 ±500ms 한계를 명시
- parser와 offset 부호 계산 unit test 추가

2026-07-17 검증 및 게시:

- local Python unit test 4개 통과
- Python syntax compile과 CLI help smoke test 통과
- draft PR #4 생성
- GitHub Actions run #29562779440에서 clang-format 19, cppcheck,
  x86_64 CMake build/CLI smoke test/Python unit test 모두 통과

도구 구현 시점에는 실제 Pi/camera/common NTP 서버 통합 측정을 수행하지 않았다.
실행 절차와 offset 정의는 `docs/time-offset-measurement-ko.md`에 기록했다.

2026-07-17 실제 read-only 통합 측정:

- Windows와 Pi가 동일한 Microsoft NTP peer `52.231.114.183`을 조회
- Windows SNTP 7/7 성공, offset 중앙값 `reference - Windows = +10.722ms`,
  RTT 중앙값 `5.223ms`
- Pi SNTP 6/7 성공, offset 중앙값 `reference - Pi = -0.306ms`,
  RTT 중앙값 `6.041ms`; 1개 표본 timeout
- 상대 offset `Pi - Windows = +11.028ms`, 추정 불확실성 `±5.632ms`
- camera `SyncType=Manual`, `camera - Pi = -2198.890ms`, SUNAPI 초 단위
  해상도를 포함한 추정 불확실성 `±531.798ms`
- 최초 수집 당시 Windows Time은 `Local CMOS Clock`, stratum 0,
  unsynchronized 상태였음
- 관리자 `w32tm /resync /rediscover` 후 source `time.windows.com,0x9`,
  stratum 5, last sync success로 복구됨
- camera 설정은 변경하지 않음

실제 JSON 응답에서 배열 형태의 camera `NTPURLList`가 누락되는 parser 결함을 발견해
scalar JSON array를 보존하도록 수정하고 unit test를 추가했다. 위 offset 계산에는
영향이 없다.

초기 통합 측정은 NTP 표본 사이를 0.2초만 기다려 RFC 4330의 SNTP client 최소 poll
간격 15초를 충족하지 못했다. 성공한 표본의 관측값은 위에 그대로 기록하되, Pi의 1개
timeout 원인을 요청 빈도로 단정하지 않는다. 도구는 2026-07-17에 기본·최소 간격을
15초로 보완했고, Pi SSH timeout도 7개 표본의 대기 시간을 포함하도록 늘렸다. 보완된
간격으로 같은 peer를 재측정하는 실제 장치 검증은 아직 수행하지 않았다.

### Raspberry Pi

2026-07-16 확인:

```text
Time zone: Asia/Seoul (KST, +0900)
System clock synchronized: yes
NTP service: active
RTC: n/a
clocksource: arch_sys_counter
chrony: not installed
```

추가 확인 결과:

```text
server=2.debian.pool.ntp.org (121.134.215.104)
stratum=2
poll interval=34min 8s
root distance=174us
offset=-6.576ms
delay=5.661ms
jitter=2.199ms
frequency=-9.539ppm
```

### Windows

2026-07-16 관리자 PowerShell에서 resync 후 확인:

```text
source=time.windows.com,0x9
stratum=5
poll interval=1024s
root delay=0.0052323s
root dispersion=7.7893297s
last sync=2026-07-16 11:26:57 KST
```

`root dispersion`은 실제 Pi와의 offset 자체가 아니다. 같은 기준 서버에 대한 offset을
별도로 측정해야 한다. Windows PowerShell 버전은 `Get-Date -AsUTC`를 지원하지 않아
`(Get-Date).ToUniversalTime().ToString("o")`를 사용한다.

`w32tm /stripchart /computer:time.windows.com` 6개 표본은 약
`+19.0ms..+20.5ms`였다. Windows와 Pi는 서로 다른 NTP server를 사용하므로 이 값과
Pi offset을 단순히 빼서 두 장치의 정확한 상대 offset으로 간주하지 않는다.

SUNAPI 2.6.8 PDF page 2462에서 camera date 조회 URI를 확인했다.

```text
/stw-cgi/system.cgi?msubmenu=date&action=view
```

카메라 실제 응답:

```text
LocalTime=2026-07-16 11:32:15
UTCTime=2026-07-16 02:32:15
SyncType=Manual
DSTEnable=false
TimeZoneIndex=83
POSIXTimeZone=STWT-9
NTPURLList=pool.ntp.org, asia.pool.ntp.org, europe.pool.ntp.org,
           north-america.pool.ntp.org, time.nist.gov
```

시간대와 UTC 변환은 맞지만 자동 동기화가 아니므로 장기 drift가 발생할 수 있다.
RTCP/NTP 기반 latency 측정 전에 camera/Pi의 NTP 기준을 통일해야 한다.

## 현재 코드 한계

- `app`은 단일 채널 연속 녹화 PoC다.
- `ChannelManager`와 상시 `ChannelIngest`는 아직 없다.
- live fan-out RTSP/RTSPS server는 아직 없다.
- recording과 live를 분리하는 `tee/queue` 구조는 아직 없다.
- SUNAPI motion session과 event recording gate는 아직 없다.
- playback/export/control plane은 아직 없다.
- `[gst] recording started`는 실제 첫 buffer 전에 출력돼 상태 표현이 부정확하다.
- GStreamer pipeline/debug 로그의 RTSP userinfo는 `<redacted>` 처리하도록 수정했으며
  실제 Pi 로그 확인은 아직 필요하다.
- SQLite는 WAL이지만 여러 recorder process용 `busy_timeout` 정책이 없다.
- 현재 Qt `delay`는 RGB frame 생성 후 GUI queue 지연만 나타낸다.

## 바로 다음 작업

Milestone M1: 시간 동기화와 지연 계측

1. **M1-A 재확인:** 15초 NTP 요청 간격으로 실제 camera/Pi/Windows 측정을 다시
   실행해 초기 결과를 확인
2. 재측정 결과를 근거로 camera `SyncType` 변경 여부와 공통 NTP 기준 결정
3. **M1-B:** GStreamer ingest 구간의 monotonic timestamp와 latency 지표 추가
4. Qt에서 packet receive, decode complete, convert complete, GUI render를 monotonic
   timestamp로 분리
5. camera direct / existing proxy 경로의 baseline latency 측정
6. 이후 단일 채널 `ChannelIngest` 구현

바로 다음 task는 `[RPi-VMS M1-A] 15초 간격 offset 재측정 및 NTP 기준 결정`이다.
보완된 간격으로 초기 측정값을 재확인하고 공통 NTP 기준안을 결정한다. 실제 camera
NTP 설정 변경은 재측정 결과와 SUNAPI 문서를 검토한 뒤 별도로 승인받아 수행한다.

Git baseline과 CI 구성은 2026-07-16에 완료했다. `VEDA-NoC/RPI-Server`는 Public
소스 저장소로 운영하며 실제 장치 설정과 credential은 추적하지 않는다.

## 관련 문서

- `docs/vms-development-roadmap-ko.md`
- `docs/latency-measurement-ko.md`
- `docs/sunapi-event-recording.md`
- `docs/git-ci-plan-ko.md`

## 개발 도구 상태

```text
Windows WSL2 Ubuntu=running
WSL clang-format=18.1.3
Windows Visual Studio clang-format=19.1.1
Pi clang-format=19.1.7
Git repository=initialized at outputs/rpi-vms
GitHub repository=VEDA-NoC/RPI-Server
GitHub visibility=public
GitHub Actions=format, cppcheck, x86_64 build, CLI smoke test
GitHub Actions execution=run #3 passed and PR #1 merged
main protection=3 required checks, strict update, 1 approval, stale review dismissal
main protection=conversation resolution required, force push/delete disabled, admin bypass allowed
GitHub Codex connector=connected as Vlolet with repository admin/push access
Confluence connector=not configured yet
```

현재 개발 루프는 Windows source를 `rsync`로 Pi의 `~/rpi-vms`에 반영하고 Pi에서
native build 및 camera/USB 시험을 수행하는 방식이다. `tools/sync-to-pi.sh`가
`.git`, `.github`, runtime data, credential과 로컬 환경 문서를 제외하며
`--dry-run`, `--clean`, `--build` 모드를 제공한다.
