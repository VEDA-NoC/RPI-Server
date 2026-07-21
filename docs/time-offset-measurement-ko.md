# M1-A 시간 상태 및 offset 측정

## 목적과 범위

`tools/measure_time_offsets.py`는 카메라 설정을 변경하지 않고 다음 값을 한 번에
수집한다.

- Windows Time 상태
- Raspberry Pi의 `tools/check-time-sync.sh` 출력
- 지정한 하나의 NTP 서버에 대한 Windows/Pi SNTP offset과 왕복 지연
- Hanwha SUNAPI date 조회 결과
- Windows, Pi, 카메라 사이의 추정 상대 offset

이 값들은 장치 wall clock 차이를 확인하기 위한 것이며 영상 end-to-end latency가
아니다. 영상 목표 `T7 - T0 <= 200ms`와 직접 비교하지 않는다.

카메라에는 다음 read-only 요청만 전송한다.

```text
GET /stw-cgi/system.cgi?msubmenu=date&action=view
```

카메라 비밀번호는 대화형으로 입력하며 명령행, JSON 결과, Git 파일에 저장하지 않는다.
도구는 NTP 설정 변경, `w32tm /resync`, `timedatectl set-*`, SUNAPI `action=update`를
실행하지 않는다.

## 1. WSL2 Ubuntu - Pi 동기화 및 원격 빌드

전체 측정 전에 최신 Windows source를 Pi에 반드시 반영한다. 이 단계는 Windows
PowerShell이나 Pi SSH shell이 아니라 **WSL2 Ubuntu**에서 실행한다. agent가 배포를
수행한 경우 사용자가 반복할 필요가 없다.

활성 저장소 루트에서 실행한다.

```bash
bash tools/sync-to-pi.sh --dry-run
bash tools/sync-to-pi.sh --build
```

정상이면 dry-run에서 전송 예정 파일이 표시되고, 실제 실행에서 source 동기화 후 Pi의
`make clean && make`가 성공한다. Python 도구 변경 자체에는 C++ build가 필요하지
않지만, 이 명령은 배포 source와 현재 Pi build가 함께 정상인지 확인한다.

Windows와 Pi에서 UDP/123으로 접근 가능한 동일 NTP 서버 하나를 정한다. 이 서버는
이번 측정의 공통 기준일 뿐이며, 장치의 현재 NTP 설정을 변경하지 않는다. DNS
anycast/pool 이름은 두 장치에서 서로 다른 서버로 해석될 수 있으므로 정밀 비교에는
가능하면 같은 서버 IP 또는 같은 사내 NTP 주소를 사용한다.

## 2. Windows 관리자 PowerShell - 전체 측정

이 단계만 사용자가 실행한다. 시작 메뉴에서 PowerShell을 **관리자 권한으로 실행**하고
저장소 디렉터리로 이동한 뒤 Python launcher를 사용한다. 일반 PowerShell에서는
환경에 따라 `w32tm /query`가 `0x80070005`로 거부될 수 있다. Pi SSH shell에서는
전체 collector를 실행하지 않는다.

```powershell
py -3 tools/measure_time_offsets.py `
  --camera-host CAMERA_IP `
  --camera-user admin `
  --pi-target noc@noc `
  --ntp-server NTP_SERVER_OR_IP `
  --samples 7 `
  --ntp-interval 15
```

실행 후 SUNAPI 비밀번호 prompt가 나타난다. 정상 완료 시 Git에서 제외되는
`measurements/time-offset-YYYYMMDDTHHMMSSZ.json`에 결과를 저장하고 핵심 상대
offset을 콘솔에 출력한다. RFC 4330의 SNTP client poll 제한에 따라 NTP 연속 요청은
기본 15초 간격이며 더 짧게 설정할 수 없다. 7개 표본을 Windows와 Pi에서 순차 수집하고
camera를 조회하므로 전체 실행 시간은 약 3~4분이다. 실행 중 `[1/3] Windows`,
`[2/3] Raspberry Pi`, `[3/3] Camera` 단계와 15초 대기 상태가 stderr에 표시된다.

## 3. Raspberry Pi SSH shell - Pi probe 단독 진단

전체 측정이 Pi 단계에서 실패할 때만 Pi SSH shell에서 다음을 사용한다. 정상 측정
절차에서는 실행할 필요가 없다.

```bash
python3 tools/measure_time_offsets.py \
  --local-probe \
  --ntp-server NTP_SERVER_OR_IP \
  --samples 7 \
  --ntp-interval 15
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

SUNAPI `UTCTime`은 관측 응답에서 초 단위이므로 카메라 offset의 하한 불확실성에는
±500ms와 HTTP 왕복시간 절반이 포함된다. SUNAPI 2.6.8 Date Information은 요청 처리
중 `UTCTime`을 캡처하는 정확한 시점을 정의하지 않으므로 camera-side systematic
error는 이 수치에 포함되지 않는다. 이 측정은 카메라 drift와 큰 시각 오차를 판별하는
용도이며, 밀리초 단위 카메라 media timestamp와 영상 end-to-end 검증은 M1-B의 RTCP
Sender Report 계측 및 glass-to-glass 측정으로 수행한다.

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
