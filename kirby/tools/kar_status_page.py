"""Builds the live status page of the Kirby Air Riders port from RELEASE_PLAN.md, status/metrics.json and the git log
of the port's repository. Writes E:/kirby/status/index.html; publish it with the Artifact tool (same file path keeps
the same URL).

    python tools/kar_status_page.py
"""
import html
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLAN = ROOT / 'RELEASE_PLAN.md'
METRICS = ROOT / 'status' / 'metrics.json'
REPO = ROOT / 'dkb-pc-recomp'
OUT = ROOT / 'status' / 'index.html'

STATES = [('✅', 'done', 'Erledigt'), ('🔧', 'work', 'In Arbeit'), ('🔶', 'part', 'Teilweise'),
          ('⬜', 'open', 'Offen'), ('❌', 'fail', 'Verworfen'), ('⏸', 'hold', 'Zurückgestellt')]


def clean(s):
    s = re.sub(r'\*\*(.+?)\*\*', r'\1', s)
    s = re.sub(r'`([^`]*)`', r'\1', s)
    return s.strip()


def parse_plan():
    sections, cur = [], None
    order = []
    in_order = False
    for line in PLAN.read_text(encoding='utf-8').splitlines():
        m = re.match(r'## (\d+)\. (.+)', line)
        if m:
            cur = {'num': m.group(1), 'title': clean(m.group(2)), 'items': []}
            sections.append(cur)
            in_order = False
            continue
        if line.startswith('## '):
            cur = None
            in_order = line.startswith('## Reihenfolge')
            continue
        if in_order:
            m = re.match(r'(\d+)\. (.+)', line)
            if m:
                order.append(clean(m.group(2)))
            continue
        if cur is None or not line.startswith('| ') or line.startswith('|---'):
            continue
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if len(cells) < 3 or not re.match(r'^\d+\.\d+[a-z]?$', cells[0]):
            continue
        last = cells[-1]
        state = next((s for s in STATES if last.startswith(s[0])), None)
        if state is None:
            state = next((s for s in STATES if s[0] in last[:4]), STATES[3])
        note = clean(last[len(state[0]):]) if last.startswith(state[0]) else clean(last)
        cur['items'].append({'id': cells[0], 'title': clean(cells[1]), 'state': state, 'note': note})
    return sections, order


def git_log(n=14):
    try:
        out = subprocess.run(['git', '-C', str(REPO), 'log', f'-{n}', '--date=format:%d.%m. %H:%M', '--pretty=%h\t%ad\t%s'],
                             capture_output=True, text=True, encoding='utf-8', timeout=20).stdout
    except Exception:
        return []
    return [line.split('\t', 2) for line in out.splitlines() if line.count('\t') >= 2]


def short(s, n=260):
    s = s.strip()
    return s if len(s) <= n else s[:n].rsplit(' ', 1)[0] + ' …'


def render():
    sections, order = parse_plan()
    metrics = json.loads(METRICS.read_text(encoding='utf-8'))
    commits = git_log()
    all_items = [i for s in sections for i in s['items']]
    counts = {k: sum(1 for i in all_items if i['state'][1] == k) for _, k, _ in STATES}
    total = len(all_items)
    e = html.escape

    def pill(state):
        return f'<span class="pill {state[1]}">{e(state[2])}</span>'

    active = [i for i in all_items if i['state'][1] in ('work', 'part')]
    h = []
    h.append('''<title>Kirby Air Riders Port</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans:wght@400;500;600&family=IBM+Plex+Mono:wght@400;500&family=Space+Grotesk:wght@500;700&display=swap">
<style>
/* Layout: a status board, summary first (focus, numbers, counts), then the sections as lists of task rows */
:root {
  --bg: #f6f5f8; --surface: #ffffff; --line: #e3dfe8; --fg: #1f1b24; --muted: #6b6475; --accent: #c2367a;
  --done: #1e8a52; --done-bg: #e3f4ea; --work: #b25c00; --work-bg: #fdf0df; --part: #8a6a00; --part-bg: #f8f1d6;
  --open: #6b6475; --open-bg: #eeebf2; --fail: #b3261e; --fail-bg: #fbe5e3; --hold: #4a5b8a; --hold-bg: #e5e9f5;
  --display: "Space Grotesk", "Segoe UI", system-ui, sans-serif; --body: "IBM Plex Sans", "Segoe UI", system-ui, sans-serif;
  --mono: "IBM Plex Mono", ui-monospace, Consolas, monospace;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg: #16131a; --surface: #1f1b24; --line: #322c3a; --fg: #ece8f1; --muted: #a39cad; --accent: #f06aa8;
  --done: #5fd193; --done-bg: #173226; --work: #f3a64b; --work-bg: #3a2814; --part: #e3c55a; --part-bg: #352e14;
  --open: #a39cad; --open-bg: #2a2531; --fail: #f2847b; --fail-bg: #3b1d1b; --hold: #9fb2ea; --hold-bg: #222a40; color-scheme: dark } }
:root[data-theme="dark"] {
  --bg: #16131a; --surface: #1f1b24; --line: #322c3a; --fg: #ece8f1; --muted: #a39cad; --accent: #f06aa8;
  --done: #5fd193; --done-bg: #173226; --work: #f3a64b; --work-bg: #3a2814; --part: #e3c55a; --part-bg: #352e14;
  --open: #a39cad; --open-bg: #2a2531; --fail: #f2847b; --fail-bg: #3b1d1b; --hold: #9fb2ea; --hold-bg: #222a40; color-scheme: dark }
body { background: var(--bg); color: var(--fg); font: 15px/1.5 var(--body); }
.wrap { max-width: 1080px; margin: 0 auto; padding-block: 28px 56px;
  padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px));
  display: grid; gap: 28px; }
h1 { font: 700 30px/1.15 var(--display); margin: 0; text-wrap: balance; }
h2 { font: 600 18px/1.3 var(--display); margin: 0; text-wrap: balance; }
.sub { color: var(--muted); margin: 6px 0 0; }
.focus { background: var(--surface); border: 1px solid var(--line); border-left: 4px solid var(--accent); border-radius: 10px; padding: 14px 16px; }
.focus .lbl, .lbl { font: 500 11px/1.4 var(--mono); letter-spacing: .08em; text-transform: uppercase; color: var(--muted); }
.focus p { margin: 4px 0 0; font-weight: 500; }
.metrics { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 12px; }
.metric { background: var(--surface); border: 1px solid var(--line); border-radius: 10px; padding: 12px 14px; min-width: 0; }
.metric .v { font: 700 26px/1.1 var(--display); font-variant-numeric: tabular-nums; margin-top: 4px; }
.metric .v small { font: 500 13px var(--body); color: var(--muted); margin-left: 4px; }
.metric .n { color: var(--muted); font-size: 13px; margin-top: 2px; }
.counts { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
.bar { display: flex; height: 10px; border-radius: 6px; overflow: hidden; background: var(--open-bg); flex: 1 1 260px; min-width: 0; }
.bar span { display: block; height: 100%; }
.pill { display: inline-block; font: 500 12px/1 var(--body); padding: 5px 9px; border-radius: 999px; white-space: nowrap; }
.done { color: var(--done); background: var(--done-bg); } .work { color: var(--work); background: var(--work-bg); }
.part { color: var(--part); background: var(--part-bg); } .open { color: var(--open); background: var(--open-bg); }
.fail { color: var(--fail); background: var(--fail-bg); } .hold { color: var(--hold); background: var(--hold-bg); }
.cols { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 340px); gap: 28px; align-items: start; }
@media (max-width: 860px) { .cols { grid-template-columns: minmax(0, 1fr); } }
.list { display: grid; gap: 1px; background: var(--line); border: 1px solid var(--line); border-radius: 10px; overflow: hidden; }
.row { display: grid; grid-template-columns: 52px minmax(0, 1fr) auto; gap: 10px; padding: 10px 12px; background: var(--surface); align-items: start; }
.row .id { font: 500 13px/1.6 var(--mono); color: var(--muted); }
.row .t { min-width: 0; }
.row .t b { font-weight: 600; }
.row .t p { margin: 3px 0 0; color: var(--muted); font-size: 13px; overflow-wrap: anywhere; }
details.sec { background: var(--surface); border: 1px solid var(--line); border-radius: 10px; }
details.sec > summary { cursor: pointer; padding: 12px 14px; display: flex; gap: 12px; align-items: center; list-style: none; }
details.sec > summary::-webkit-details-marker { display: none; }
details.sec > summary:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; border-radius: 10px; }
details.sec > summary .st { flex: 1 1 auto; min-width: 0; font-weight: 600; }
details.sec > summary .mini { flex: 0 0 120px; height: 6px; border-radius: 4px; background: var(--open-bg); overflow: hidden; }
details.sec > summary .mini span { display: block; height: 100%; background: var(--done); }
details.sec > summary .cnt { font: 500 12px var(--mono); color: var(--muted); font-variant-numeric: tabular-nums; }
details.sec .list { border: 0; border-top: 1px solid var(--line); border-radius: 0 0 10px 10px; }
.stack { display: grid; gap: 10px; }
.side { display: grid; gap: 20px; }
ol.order { margin: 0; padding-left: 20px; display: grid; gap: 6px; color: var(--fg); font-size: 14px; }
.commits { display: grid; gap: 8px; font-size: 13px; }
.commits div { display: grid; grid-template-columns: auto minmax(0, 1fr); gap: 2px 8px; }
.commits code { font: 500 12px var(--mono); color: var(--accent); }
.commits time { font: 12px var(--mono); color: var(--muted); }
.commits span { grid-column: 1 / -1; overflow-wrap: anywhere; }
@media (prefers-reduced-motion: reduce) { * { transition: none !important; } }
</style>''')
    h.append('<div class="wrap">')
    h.append(f'<header><h1>Kirby Air Riders Port</h1><p class="sub">Stand der Aufgabenliste · zuletzt aktualisiert '
             f'{e(metrics.get("updated", ""))} · {total} Punkte</p></header>')
    h.append(f'<div class="focus"><div class="lbl">Gerade in Arbeit</div><p>{e(metrics.get("focus", ""))}</p></div>')
    h.append('<section class="stack"><div class="lbl">Kennzahlen</div><div class="metrics">')
    for m in metrics.get('metrics', []):
        h.append(f'<div class="metric"><div class="lbl">{e(m["label"])}</div><div class="v">{e(m["value"])}'
                 f'<small>{e(m.get("unit", ""))}</small></div><div class="n">{e(m.get("note", ""))}</div></div>')
    h.append('</div></section>')
    # overall counts
    h.append('<section class="stack"><div class="lbl">Gesamtfortschritt</div><div class="counts"><div class="bar">')
    for _, k, _ in STATES:
        if counts[k]:
            h.append(f'<span class="{k}" style="width:{100 * counts[k] / max(total, 1):.2f}%;background:var(--{k})" '
                     f'title="{e(dict((s[1], s[2]) for s in STATES)[k])}: {counts[k]}"></span>')
    h.append('</div>')
    for sym, k, name in STATES:
        if counts[k]:
            h.append(f'<span class="pill {k}">{e(name)} {counts[k]}</span>')
    h.append('</div></section>')
    h.append('<div class="cols"><main class="stack">')
    if active:
        h.append('<div class="lbl">Aktive Punkte</div><div class="list">')
        for i in active:
            h.append(f'<div class="row"><div class="id">{e(i["id"])}</div><div class="t"><b>{e(short(i["title"], 140))}</b>'
                     f'<p>{e(short(i["note"]))}</p></div>{pill(i["state"])}</div>')
        h.append('</div>')
    h.append('<div class="lbl" style="margin-top:8px">Alle Abschnitte</div>')
    for s in sections:
        n = len(s['items'])
        d = sum(1 for i in s['items'] if i['state'][1] == 'done')
        if not n:
            continue
        h.append(f'<details class="sec"><summary><span class="st">{e(s["num"])}. {e(s["title"])}</span>'
                 f'<span class="mini"><span style="width:{100 * d / n:.1f}%"></span></span><span class="cnt">{d}/{n}</span></summary><div class="list">')
        for i in s['items']:
            note = f'<p>{e(short(i["note"]))}</p>' if i['note'] else ''
            h.append(f'<div class="row"><div class="id">{e(i["id"])}</div><div class="t"><b>{e(short(i["title"], 160))}</b>'
                     f'{note}</div>{pill(i["state"])}</div>')
        h.append('</div></details>')
    h.append('</main><aside class="side">')
    if order:
        h.append('<section class="stack"><div class="lbl">Reihenfolge</div><ol class="order">')
        for o in order:
            h.append(f'<li>{e(short(o, 220))}</li>')
        h.append('</ol></section>')
    if commits:
        h.append('<section class="stack"><div class="lbl">Letzte Commits (iiVoiX/kar)</div><div class="commits">')
        for c in commits:
            h.append(f'<div><code>{e(c[0])}</code><time>{e(c[1])}</time><span>{e(c[2])}</span></div>')
        h.append('</div></section>')
    h.append('</aside></div></div>')
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text('\n'.join(h), encoding='utf-8')
    print(f'{OUT}: {total} items, {len(commits)} commits')


if __name__ == '__main__':
    render()
