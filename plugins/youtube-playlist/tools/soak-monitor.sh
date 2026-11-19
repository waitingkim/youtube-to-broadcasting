#!/bin/zsh
# 장시간 송출 테스트용 모니터.
# 실행 중인 OBS의 메모리·스레드·열린 파일 수·CPU를 주기적으로 CSV로 기록하고,
# 끝나면 YouTube 재생목록 플러그인 로그에서 재생/실패 횟수를 요약합니다.
#
# 사용법:
#   plugins/youtube-playlist/tools/soak-monitor.sh [시간(초)] [간격(초)] [출력.csv]
#   예) 24시간, 1분 간격:  plugins/youtube-playlist/tools/soak-monitor.sh 86400 60 ~/obs-soak.csv
#
# OBS가 여러 개 떠 있으면 OBS_PID 환경 변수로 대상을 지정하세요.

DUR=${1:-86400}
INT=${2:-60}
OUT=${3:-$HOME/obs-soak-$(date +%Y%m%d-%H%M).csv}
PID=${OBS_PID:-$(pgrep -x OBS | head -1)}

if [[ -z "$PID" ]]; then
  echo "실행 중인 OBS를 찾지 못했습니다." >&2
  exit 1
fi

echo "OBS pid $PID 를 ${DUR}초 동안 ${INT}초 간격으로 기록합니다 → $OUT"
echo "elapsed_s,rss_mb,threads,open_files,cpu_pct" > "$OUT"
START=$(date +%s)
while kill -0 "$PID" 2>/dev/null; do
  EL=$(( $(date +%s) - START ))
  RSS=$(ps -o rss= -p "$PID" | tr -d ' ')
  THR=$(ps -M -p "$PID" | tail -n +2 | wc -l | tr -d ' ')
  FDS=$(lsof -p "$PID" 2>/dev/null | wc -l | tr -d ' ')
  CPU=$(ps -o %cpu= -p "$PID" | tr -d ' ')
  echo "$EL,$((RSS / 1024)),$THR,$FDS,$CPU" >> "$OUT"
  (( EL >= DUR )) && break
  sleep "$INT"
done

echo
echo "== 요약 =="
awk -F, 'NR==2{r0=$2;t0=$3;f0=$4} NR>1{r=$2;t=$3;f=$4; if($2>rmax)rmax=$2}
  END{printf "메모리: 시작 %d MB → 끝 %d MB (최대 %d MB)\n스레드: %d → %d\n열린 파일: %d → %d\n", r0,r,rmax,t0,t,f0,f}' "$OUT"

LOG_DIR="$HOME/Library/Application Support/obs-studio/logs"
LOG="$LOG_DIR/$(ls -t "$LOG_DIR" 2>/dev/null | head -1)"
if [[ -f "$LOG" ]]; then
  echo "재생 시작: $(grep -c "\[youtube-playlist: .*Playing item" "$LOG")회"
  echo "재생 실패: $(grep -c "\[youtube-playlist: .*\(Failed to play\|could not be fetched\)" "$LOG")회"
  echo "다운로드 대기: $(grep -c "\[youtube-playlist: .*Waiting for download" "$LOG")회"
  echo "오류 후 재시도: $(grep -c "\[youtube-playlist: .*Retrying the playlist" "$LOG")회"
fi
echo
echo "누수 확인(선택): leaks $PID | tail -5"
