# Extract one report day (YYMMDD 07:00 KST to the next day 07:00) from the Claude Code transcripts of this repo:
# user requests (typed or queued mid-turn) and the assistant's turn summaries, in time order, as markdown.
# usage: python3 daylog.py YYMMDD > body.md        (exit 1 when the day has no requests)
import glob
import json
import os
import re
import sys
from datetime import datetime, timedelta

MARK = '[daily-report-auto]'   # prompt marker of daily_report.sh: its own sessions are skipped
D = os.path.expanduser('~/.claude/projects/-home-ubuntu-npuprg-nestc2-clion-nest-compiler/')
day = datetime.strptime(sys.argv[1], '%y%m%d')
lo = (day + timedelta(hours=7 - 9)).strftime('%Y-%m-%dT%H:%M')   # transcript timestamps are UTC
hi = (day + timedelta(hours=31 - 9)).strftime('%Y-%m-%dT%H:%M')


def skip_user(x):
    return (x.startswith('<') or x.startswith('This session is being continued')
            or x.startswith('[Request interrupted'))


ev = []
for f in sorted(glob.glob(D + '*.jsonl')):
    sid = os.path.basename(f)[:8]
    rows = []
    auto = False
    for line in open(f, errors='replace'):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        m = d.get('message') or {}
        c = m.get('content')
        if d.get('type') == 'user' and isinstance(c, str) and c.startswith(MARK):
            auto = True
            break
        t = d.get('timestamp', '')
        if not (lo <= t < hi):
            continue
        k = datetime.fromisoformat(t[:19]) + timedelta(hours=9)
        if d.get('type') == 'user' and m.get('role') == 'user':
            txts = [c] if isinstance(c, str) else [b.get('text', '') for b in c or [] if b.get('type') == 'text']
            rows += [(k, sid, 'U', x.strip()) for x in txts if x.strip() and not skip_user(x.strip())]
        elif d.get('type') == 'attachment' and isinstance(d.get('attachment'), dict) \
                and d['attachment'].get('type') == 'queued_command':
            x = str(d['attachment'].get('prompt', '')).strip()
            if x and not skip_user(x):
                rows.append((k, sid, 'U', x))
        elif m.get('role') == 'assistant' and isinstance(c, list):
            rows += [(k, sid, 'A', b['text'].strip()) for b in c if b.get('type') == 'text' and len(b['text']) > 200]
    if not auto:
        ev += rows

if not any(e[2] == 'U' for e in ev):
    sys.exit(1)
ev.sort(key=lambda e: e[0])
out = []
for k, sid, r, x in ev:
    hm = k.strftime('%H:%M') if k.hour >= 7 else k.strftime('%m-%d %H:%M')
    if r == 'U':
        x = x if len(x) < 3000 else x[:3000] + ' ...(생략)'
        out.append(f'\n### {hm} [세션 {sid}] 요청\n\n> ' + x.replace('\n', '\n> ') + '\n')
    else:
        x = re.sub(r'^#{1,6} ', '#### ', x, flags=re.M)
        out.append(f'\n**{hm} [세션 {sid}] 결과**\n\n' + x + '\n')
print(''.join(out))
