#!/bin/bash
# Daily EVTA report: for each day (07:00 KST boundary) from the day after the newest report up to yesterday,
# extract the Claude Code transcripts (daylog.py) and let `claude -p` write
# docs/dailyreport/vta_systolic/vta_systolic_요약_<YYMMDD>.md. Run from cron at 07:00:
#   0 7 * * * /home/ubuntu/npuprg/nestc2_clion/nest-compiler/docs/dailyreport/tools/daily_report.sh >> ~/.cache/daily_report.log 2>&1
# Manual: daily_report.sh [YYMMDD ...]   (regenerates the given days)
# Report set: SET=vta_systolic (default, all sessions) or SET=vta_synth (the bitstream-synthesis session only)
set -u
N=/home/ubuntu/npuprg/nestc2_clion/nest-compiler
T=$N/docs/dailyreport/tools
SET=${SET:-vta_systolic}
case $SET in
  vta_systolic) TITLE="EVTA systolic"; SESS="" ;;
  vta_synth)    TITLE="EVTA synth";    SESS="57e1dbd5" ;;
  *) echo "unknown SET $SET"; exit 1 ;;
esac
R=$N/docs/dailyreport/$SET; mkdir -p "$R"
CLAUDE=${CLAUDE:-/home/ubuntu/.local/bin/claude}
W=${TMPDIR:-/tmp}/daily_report_$USER; mkdir -p "$W"
export TZ=Asia/Seoul

if [ $# -gt 0 ]; then
  days="$*"
else
  last=$(ls "$R" 2>/dev/null | sed -n "s/^${SET}_요약_\([0-9]\{6\}\)\.md$/\1/p" | sort | tail -1)
  last=${last:-261025}
  # the report day ends at 07:00, so the newest complete day is yesterday when run at/after 07:00
  y=$(date -d '-7 hours -1 day' +%y%m%d)
  days=""
  d=$(date -d "20${last:0:2}-${last:2:2}-${last:4:2} +1 day" +%y%m%d)
  while [[ "$d" < "$y" || "$d" == "$y" ]]; do
    days="$days $d"
    d=$(date -d "20${d:0:2}-${d:2:2}-${d:4:2} +1 day" +%y%m%d)
  done
fi

for d in $days; do
  echo "== $(date '+%F %T') day $d"
  body=$W/${SET}_body_$d.md
  if ! python3 "$T/daylog.py" "$d" $SESS > "$body"; then echo "no requests on $d, skipped"; continue; fi
  out=$R/${SET}_요약_$d.md
  prompt=$(sed -e "s|{BODY}|$body|g" -e "s|{OUT}|$out|g" -e "s|{TITLE}|$TITLE|g" "$T/prompt.md")
  # hooks off: the repo's SessionEnd hook commits and pushes the whole tree; acceptEdits: the report file only
  # (the prompt names it; other tools than Read/Write/Edit/ls stay denied)
  cd "$N" && "$CLAUDE" -p "$prompt" --add-dir "$W" \
    --settings '{"disableAllHooks": true}' --permission-mode acceptEdits \
    --allowedTools "Read" "Write" "Edit" "Bash(ls:*)" \
    --output-format text > "$W/${SET}_claude_$d.log" 2>&1
  echo "claude rc=$? -> $([ -s "$out" ] && echo "$out ($(wc -l < "$out") lines)" || echo 'no report')"
done
