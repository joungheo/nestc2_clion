# 호스트 PC 갑작스런 꺼짐 (2026-10-25 ~ 11-11): 기록, 원인, 온도 보는 명령

비트스트림 합성(docker Vivado)과 PC 쪽 검증(fake-quant emulator, CSim)을 돌리는 x86 호스트 PC가 로그 없이 꺼지는 일이 되풀이됐다.
11-11에 온도를 5초마다 남긴 뒤로, **부하가 거의 없는데도 CPU 온도가 몇 분 만에 107 °C까지 올라 꺼지는 것**이 세 번 기록됐다.
수랭 쿨러의 물이 돌지 않는 것(펌프 고장)으로 본다.

## 1. PC

| 항목 | 값 |
|---|---|
| CPU | AMD Ryzen 9 5950X (16코어 32스레드) |
| 메인보드 | ASUS PRIME X570-PRO, BIOS 4002 (2021-06-15) |
| CPU 쿨러 | 일체형 수랭(모델 미확인; USB 연결이 없어 Linux에서 알 수 없음) |
| OS | Ubuntu 20.04, kernel 5.15.0-139, 설치 2022-06-08(`/lost+found` 생성 시각) |
| 구입 | 컴퓨존(박스 있음). PC 안에는 주문 기록이 없다. 2022년 6월 무렵으로 보인다 |

## 2. 꺼진 기록

`last -x -F`에서 `crash`(정상 종료 없이 끊긴 세션)와 그다음 부팅으로 정리했다. 정확한 시각은 그때 돌던 작업의 로그나 온도 로그에서 확인했다.

| # | 꺼진 시각 | 그때 하던 일 | 온도 기록 |
|---|---|---|---|
| 1 | 10-25 14:05 | Vivado multi4 합성(전 코어) | 없음 |
| 2 | 10-28 11:10 | Vivado multi4 systolic 배선(전 코어) | 없음 |
| 3 | 11-02 22:46:43 | Vivado `route_design`(전 코어) | 없음 |
| 4 | 11-03 ~ 11-09 사이(시각 불명) | 거의 유휴 | 없음 |
| 5 | 11-11 00:18 | onnxruntime emulator `fakequant.py`, 32 스레드 | 없음 |
| 6 | 11-11 10:20 | 아무것도 안 돌림(load 0.3 안팎) | 80 → 107 °C, 2분 |
| 7 | 11-11 16:07:46 | 아무것도 안 돌림(load 0.3–0.4), 부팅 5시간 43분 뒤 | 70 → 107 °C, 3분 30초 |

- 모두 journal이 그냥 끊겼다. kernel panic, MCE, OOM, thermal 로그는 없다.
- 1–5번만 볼 때는 전원부나 오래된 BIOS(Ryzen 5000의 저부하 꺼짐 문제)도 의심했다. 하지만 6, 7번은 부하가 없는데 온도만 올라서 꺼졌다.

**6번 (11-11 10:20)** `~/templog/tctl.log`, 30초 간격:

```
10:16:01 Tctl  76  load 0.30
10:17:01 Tctl  78  load 0.39
10:18:01 Tctl  80  load 0.36
10:18:31 Tctl  82  load 0.28
10:19:02 Tctl  86  load 0.39
10:19:32 Tctl  94  load 1.96
10:20:02 Tctl 103  load 1.30
(기록 끝, 107 °C 근처에서 꺼짐)
```

**7번 (11-11 16:07)** 20초 간격, 끝은 5초 간격:

```
16:04:16 Tctl  70  load 0.00
16:04:56 Tctl  74  load 0.10
16:05:36 Tctl  77  load 0.12
16:06:16 Tctl  80  load 0.15
16:06:36 Tctl  85  load 0.32
16:07:16 Tctl  92  load 0.41
16:07:31 Tctl  97  load 0.39
16:07:36 Tctl 101  load 0.36
16:07:41 Tctl 102  load 0.33
16:07:46 Tctl 107  load 0.30
(기록 끝)
```

다시 켠 뒤(16:31)에는 유휴 44–52 °C였다. 11-09에는 유휴 46.6 °C였다.

## 3. 원인 판단

**부하 없이 107 °C까지 오르는 이유**
- 5950X는 놀 때도 30–40 W를 낸다.
- 펌프가 돌면 이 열이 물을 타고 라디에이터로 가고, 팬이 식힌다.
- 펌프가 멈추면 CPU 위 냉각 블록 안의 적은 물만 데워진다. 열이 빠져나갈 길이 없으니 부하와 상관없이 계속 오른다.
- 107 °C 근처에서 보드의 과열 보호(THERMTRIP)가 전원을 끊는다. 로그를 남길 틈이 없다.

**펌프 고장으로 보는 근거**
- 몇 시간 정상이다가(유휴 45–60 °C), 부하 변화 없이 몇 분 만에 30 °C 넘게 오른다. 펌프가 가끔 멈추는 모양이다.
  - 펌프 베어링이 닳았거나 안에 공기가 찬 경우다. 4년쯤 된 일체형 수랭에서 흔하다.
- 11-11 아침에는 유휴 73–80 °C였다.
  - 전 코어 부하를 주면 2초 만에 90 °C가 됐다.
  - 부하를 멈추고 10초 뒤에도 81 °C였다.
  - 열이 라디에이터로 빠지지 않는다는 뜻이다.
- 사용자 확인: 팬 진동과 관 안에 물이 흐르는 것 같은 진동은 있다. 하지만 펌프가 헛돌거나 덜덜거려도 진동은 난다. 진동이 있다고 물이 돈다는 보장은 없다.

**아닌 것**
- 소프트웨어 부하가 아니다. 6, 7번은 gnome-shell 13 % 정도뿐이었다.
- BIOS 업데이트는 순서가 뒤다. 온도가 오르는 것은 BIOS로 고칠 수 없다.

## 4. 온도와 팬을 보는 명령

```bash
# 지금 CPU 온도 (k10temp의 Tctl)
sensors | grep -i tctl
# 또는 sysfs에서 직접 (m°C)
for h in /sys/class/hwmon/hwmon*; do echo "$(cat $h/name) $(cat $h/temp1_input 2>/dev/null)"; done

# 2초마다 계속 보기
watch -n 2 "sensors | grep -i -E 'tctl|composite'"

# 온도 기록 (아래 스크립트가 남긴 것)
tail -f ~/templog/tctl.log
tail -40 ~/templog/tctl.log          # 꺼진 뒤 마지막 기록 확인

# 꺼진 기록 (crash = 정상 종료 없이 끊김)
last -x -F | grep -E "crash|reboot" | head
journalctl --list-boots | tail -5
journalctl -b -1 -n 50                # 이전 부팅의 마지막 로그

# 팬 / 펌프 RPM: 이 PC는 기본으로 센서 드라이버가 안 올라온다 (sudo 필요)
sudo modprobe nct6775 && sensors | grep -i -E "fan|pump"
#   안 보이면 kernel 인자 acpi_enforce_resources=lax 가 필요할 수 있다. 그래도 안 되면 BIOS Monitor 화면에서 본다.

# 부하와 CPU를 쓰는 프로세스
uptime
ps -eo pcpu,pid,comm --sort=-pcpu | head
```

**온도 기록 스크립트** `~/templog/tctl_log.sh`:
- 5초마다 Tctl, NVMe 온도, load를 `~/templog/tctl.log`에 적는다.
- 적을 때마다 `sync`해서, 갑자기 꺼져도 마지막 줄이 남는다.

```sh
#!/bin/sh
K=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = k10temp ] && echo $h; done)
N=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = nvme ] && echo $h; done | head -1)
while :; do
  echo "$(date '+%F %T') Tctl $(( $(cat $K/temp1_input)/1000 )) nvme $(( $(cat $N/temp1_input)/1000 )) load $(cut -d' ' -f1 /proc/loadavg)" >> $HOME/templog/tctl.log
  sync $HOME/templog/tctl.log 2>/dev/null
  sleep 5
done
```

- **부팅마다 다시 켜야 한다.**
  ```bash
  cd ~/templog && setsid nohup ./tctl_log.sh >/dev/null 2>&1 < /dev/null &
  pgrep -af tctl_log        # 돌고 있는지
  ```
- 부팅 때 자동으로 켜려면 사용자가 직접 `crontab -e`로 아래 줄을 넣는다.
  ```
  @reboot $HOME/templog/tctl_log.sh
  ```

## 5. 할 일

1. 펌프 확인
   - BIOS Monitor에서 CPU_FAN / AIO_PUMP RPM을 본다. 0이거나 크게 흔들리면 펌프 고장이다.
   - 켠 상태에서 라디에이터 쪽 호스와 CPU 쪽 호스를 만져 본다. 온도 차이가 없고 CPU만 뜨거우면 물이 돌지 않는 것이다.
   - 펌프 전원 케이블(AIO_PUMP 헤더 또는 SATA 전원)이 제대로 꽂혀 있는지 본다.
2. 보증 확인
   - 케이스 옆판을 열고 펌프 헤드나 라디에이터에서 제조사와 모델명을 확인한다.
   - 컴퓨존 마이페이지의 주문/배송 조회에서 2022년 주문 내역을 본다.
   - 일체형 수랭은 보통 제조사 보증이 3–6년이다.
3. 교체: 공랭 듀얼 타워 쿨러(AM4)가 가장 확실하다. 수랭을 다시 쓸 경우 새 일체형으로 바꾼다.
4. 고칠 때까지
   - 자리를 비울 때는 PC를 끈다.
   - 임시로 BIOS AMD Overclocking의 Eco Mode(65 W)로 열을 줄일 수 있다. 펌프가 멈추면 소용없다.
   - 전 코어 작업(Vivado 합성, 32 스레드 emulator, 큰 빌드)은 하지 않는다.
   - 가벼운 작업도 온도를 보면서 한다. emulator는 4 스레드 이하로 돌리고, 85 °C가 넘으면 멈춘다.
   - 측정은 보드(ZCU102)에서 한다.
5. 쿨러를 고친 뒤 전 코어 부하로 확인한다. 30분 동안 85 °C 아래를 유지하고, 멈춘 뒤 바로 50 °C대로 내려와야 한다.

## 6. 영향

- 비트스트림 합성(ALU 확장 4단계, `evta_isa_ext.md` §8.5)은 쿨러를 고친 뒤로 미뤘다.
- HLS CSim(3단계)은 가볍다. 그래도 온도가 안정된 것을 확인하고 짧게 돌린다.
- 11-11 16:07 꺼짐 때 바꾸던 파일은 없었다. 코드 상태는 tag `alux-step2-261111`과 `~/npuprg/archive/alux_step2_before_hls_261111/`에 남아 있다.
