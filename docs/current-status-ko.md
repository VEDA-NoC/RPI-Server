# RPi VMS 현재 상태

마지막 갱신: 2026-07-16

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

## 시간 동기화 상태

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

1. camera/Pi 공통 NTP 기준 결정 및 camera `SyncType` 변경
2. Windows와 공통 기준 또는 상대 offset 측정
3. Qt에서 packet receive, decode complete, convert complete, GUI render를 monotonic
   timestamp로 분리
4. camera direct / existing proxy 경로의 baseline latency 측정
5. 이후 단일 채널 `ChannelIngest` 구현

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
GitHub Actions execution=not run yet
GitHub Codex connector=connected as Vlolet with repository admin/push access
Confluence connector=not configured yet
```

현재 개발 루프는 Windows source를 `rsync`로 Pi의 `~/rpi-vms`에 반영하고 Pi에서
native build 및 camera/USB 시험을 수행하는 방식이다.
