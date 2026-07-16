# M1-A 시간 상태 및 offset 측정

## 목적과 범위

`tools/measure_time_offsets.py`는 카메라 설정을 변경하지 않고 다음 값을 한 번에
수집한다.

- Windows Time 상태
- Raspberry Pi의 `tools/check-time-sync.sh` 출력
- 지정한 하나의 NTP 서버에 대한 Windows/Pi SNTP offset과 왕복 지연
- Hanwha SUNAPI date 조회 결과
- Windows, Pi, 카메라 사이의 추정 상대 offset

카메라에는 다음 read-only 요청만 전송한다.

```text
GET /stw-cgi/system.cgi?msubmenu=date&action=view
```

카메라 비밀번호는 대화형으로 입력하며 명령행, JSON 결과, Git 파일에 저장하지 않는다.
도구는 NTP 설정 변경, `w32tm /resync`, `timedatectl set-*`, SUNAPI `action=update`를
실행하지 않는다.

## 측정 전 준비

Windows 저장소를 Pi에 먼저 동기화한다.

```bash
cd /mnt/c/Users/shini/Documents/Codex/2026-07-10/rtsps-codex-hanwha-rtsp-raspberry-pi/outputs/rpi-vms
bash tools/sync-to-pi.sh --dry-run
bash tools/sync-to-pi.sh
```

Windows와 Pi에서 UDP/123으로 접근 가능한 동일 NTP 서버 하나를 정한다. 이 서버는
이번 측정의 공통 기준일 뿐이며, 장치의 현재 NTP 설정을 변경하지 않는다. DNS
anycast/pool 이름은 두 장치에서 서로 다른 서버로 해석될 수 있으므로 정밀 비교에는
가능하면 같은 서버 IP 또는 같은 사내 NTP 주소를 사용한다.

## 실행

관리자 Windows PowerShell에서 Python launcher를 사용한다. 관리자 권한이 없으면
환경에 따라 `w32tm /query` 상태 수집이 `0x80070005`로 거부될 수 있다.

```powershell
py -3 tools/measure_time_offsets.py `
  --camera-host CAMERA_IP `
  --camera-user admin `
  --pi-target noc@noc `
  --ntp-server NTP_SERVER_OR_IP `
  --samples 7
```

실행 후 SUNAPI 비밀번호 prompt가 나타난다. 정상 완료 시 Git에서 제외되는
`measurements/time-offset-YYYYMMDDTHHMMSSZ.json`에 결과를 저장하고 핵심 상대
offset을 콘솔에 출력한다.

Pi probe만 독립 확인할 때는 다음을 사용한다.

```bash
python3 tools/measure_time_offsets.py \
  --local-probe \
  --ntp-server NTP_SERVER_OR_IP \
  --samples 7
```

## offset 정의

개별 NTP 표본의 `offset_ms`는 다음 정의를 사용한다.

```text
offset = reference NTP clock - local device clock
```

최종 `relative_offsets`는 다음 정의를 사용한다.

```text
left_clock_minus_right_clock
```

따라서 `pi_minus_windows_ms`가 양수면 Pi 시계가 Windows보다 앞서 있다.

```text
pi_minus_windows = windows_ntp_offset - pi_ntp_offset
camera_minus_reference = camera_minus_windows - windows_ntp_offset
camera_minus_pi = camera_minus_windows - pi_minus_windows
```

SUNAPI `UTCTime`은 관측 응답에서 초 단위이므로 카메라 offset에는 기본적으로
±500ms와 HTTP 왕복시간 절반의 불확실성이 있다. 이 측정은 카메라 drift와 큰 시각
오차를 판별하는 용도이며, 밀리초 단위 카메라 media timestamp 검증은 M1-B의
RTCP Sender Report 계측으로 수행한다.

## 실패 시 확인

NTP 표본이 모두 실패하면 양쪽 장치에서 UDP/123 방화벽, DNS 결과, 서버 응답 여부를
확인한다.

```powershell
Resolve-DnsName NTP_SERVER_OR_IP
Test-NetConnection NTP_SERVER_OR_IP -Port 123 -InformationLevel Detailed
```

`Test-NetConnection`의 TCP 123 결과만으로 UDP NTP 성공을 판정하지 않는다. 실제
판정 근거는 도구의 SNTP 응답이다.

Pi SSH probe 실패 시:

```powershell
ssh noc@noc "cd /home/noc/rpi-vms && python3 tools/measure_time_offsets.py --local-probe --ntp-server NTP_SERVER_OR_IP --samples 3"
```

SUNAPI 실패 시 카메라 host, Digest 계정 권한, HTTP/HTTPS scheme을 확인한다.
HTTPS 카메라는 `--camera-scheme https`를 사용하되, 현재 도구는 인증서 검증을
비활성화하지 않는다.
