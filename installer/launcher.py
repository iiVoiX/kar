"""Kirby Air Riders Recomp launcher (RELEASE_PLAN 13.2c): play button, status and the main settings, in the port's own
pastel style (original drawing, no game artwork). Kirby only; the installer window (gui.py) stays as it is.

    python -m installer.launcher [--dir <install folder>] [--lang de|en|ru] [--selftest]

--dir defaults to the folder holding kar.exe beside this checkout's build (build/kar458752) or the current folder.
Settings are lines in <dir>/dkb.ini (the file kar.exe reads, runtime/cfg.c): each choice sets one key and every other
line stays. Without kar.exe in --dir the launcher offers the installer instead of the play button.
--selftest checks every key and value against runtime/cfg.c's table and exits (0 = all known).
"""
import argparse
import os
import re
import subprocess
import sys
import tkinter as tk
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# colours (pre-blended: tkinter has no alpha)
GROUND, CIRCLE, ACCENT, ACCENT_DARK = '#ffd7e6', '#ffe9f1', '#ee5a9b', '#b4236a'
INK, INK_SOFT, MUTED, STAR = '#2b2350', '#4a4170', '#6b5f94', '#ffd23f'
CARD, LINE, TINT, OK, WARN = '#ffffff', '#ecdcf0', '#fff4f9', '#2fa36b', '#e0a100'
FONT = 'Segoe UI' if os.name == 'nt' else 'DejaVu Sans'

# (key in dkb.ini, cfg.c name, section, [(value, label id)]); labels are text ids below or literal names
TABS = [
    ('display', [
        ('fullscreen', 'FULLSCREEN', 'display', [('1', 'on'), ('0', 'window')]),
        ('ultrawide', 'ULTRAWIDE', 'display', [('1', 'on'), ('0', 'off')]),
        ('hud_position', 'HUD_POSITION', 'display', [('centered', 'centre'), ('edges', 'edges')]),
        ('fps_limit', 'FPS_LIMIT', 'frame rate', [('60', '60'), ('120', '120'), ('144', '144'), ('240', '240'),
                                                  ('unlimited', 'unlimited')]),
    ]),
    ('graphics', [
        ('upscaler', 'UPSCALER', 'display', [('fsr', 'AMD FSR 1'), ('fsr3', 'AMD FSR 3.1'), ('external', 'NVIDIA DLSS')]),
        ('frame_gen', 'FRAME_GEN', 'display', [('0', 'off'), ('1', 'AMD FSR 3'), ('dlss', 'NVIDIA DLSS')]),
    ]),
    ('controls', [
        ('pad_layout', 'PAD_LAYOUT', 'input', [('auto', 'auto'), ('nintendo', 'Nintendo'), ('xbox', 'Xbox')]),
        ('button_glyphs', 'BUTTON_GLYPHS', 'input', [('auto', 'auto'), ('switch', 'Switch'), ('xbox', 'Xbox'),
                                                     ('playstation', 'PlayStation'), ('keyboard', 'keyboard')]),
        ('keyboard_player', 'KEYBOARD_PLAYER', 'input', [('1', 'player1'), ('2', 'player2')]),
        ('motion', 'MOTION', 'input', [('1', 'on'), ('0', 'off')]),
    ]),
    ('audio', [
        ('volume', 'VOLUME', 'audio', [('50', '50 %'), ('75', '75 %'), ('100', '100 %')]),
        ('mute_unfocused', 'MUTE_UNFOCUSED', 'audio', [('1', 'on'), ('0', 'off')]),
    ]),
]
DLSS_VALUES = {('upscaler', 'external'), ('frame_gen', 'dlss')}   # only with the DLSS add-on beside kar.exe
DEFAULTS = {'fullscreen': '0', 'ultrawide': '0', 'hud_position': 'edges', 'fps_limit': '60', 'upscaler': 'fsr',
            'frame_gen': '0', 'pad_layout': 'auto', 'button_glyphs': 'auto', 'keyboard_player': '1', 'motion': '1',
            'volume': '100', 'mute_unfocused': '0'}

TEXT = {
    'en': {'tag': 'NATIVE PC PORT', 'sub': 'Your own dump, running natively on the PC.', 'play': 'PLAY',
           'running': 'RUNNING…', 'install': 'INSTALL', 'status': 'STATUS', 'exe': 'Game ready', 'noexe': 'Not installed yet',
           'dlss': 'DLSS add-on found', 'nodlss': 'No DLSS add-on', 'cache': 'Shader cache ready', 'nocache': 'Shaders build on first start',
           'folder': 'Open game folder', 'logs': 'Open log', 'reinstall': 'Reinstall', 'saved': 'Saved',
           'foot': 'Changes apply on the next start. Everything else in the game with F1.',
           'display': 'Display', 'graphics': 'Graphics', 'controls': 'Controls', 'audio': 'Audio',
           'fullscreen': 'Fullscreen', 'ultrawide': 'Ultrawide', 'hud_position': 'HUD position (ultrawide)',
           'fps_limit': 'Frame-rate limit', 'upscaler': 'Upscaler', 'frame_gen': 'Frame generation',
           'pad_layout': 'Button layout', 'button_glyphs': 'Button symbols', 'keyboard_player': 'Keyboard plays as',
           'motion': 'Motion controls', 'volume': 'Volume', 'mute_unfocused': 'Mute in background',
           'on': 'On', 'off': 'Off', 'window': 'Window', 'centre': 'Centre', 'edges': 'Edges', 'unlimited': 'Unlimited',
           'auto': 'Automatic', 'keyboard': 'Keyboard', 'player1': 'Player 1', 'player2': 'Player 2'},
    'de': {'tag': 'PC-PORT', 'sub': 'Dein eigener Dump, nativ auf dem PC.', 'play': 'SPIELEN', 'running': 'LÄUFT…',
           'install': 'INSTALLIEREN', 'status': 'STATUS', 'exe': 'Spiel bereit', 'noexe': 'Noch nicht installiert',
           'dlss': 'DLSS-Add-on gefunden', 'nodlss': 'Kein DLSS-Add-on', 'cache': 'Shader-Cache bereit',
           'nocache': 'Shader entstehen beim ersten Start', 'folder': 'Spielordner öffnen', 'logs': 'Protokoll öffnen',
           'reinstall': 'Neu installieren', 'saved': 'Gespeichert',
           'foot': 'Änderungen gelten beim nächsten Start. Alles Weitere im Spiel per F1.',
           'display': 'Anzeige', 'graphics': 'Grafik', 'controls': 'Steuerung', 'audio': 'Ton',
           'fullscreen': 'Vollbild', 'ultrawide': 'Ultrawide', 'hud_position': 'HUD-Position (Ultrawide)',
           'fps_limit': 'Bildratenbegrenzung', 'upscaler': 'Skalierer', 'frame_gen': 'Bildgenerierung',
           'pad_layout': 'Tastenbelegung', 'button_glyphs': 'Tastensymbole', 'keyboard_player': 'Tastatur spielt als',
           'motion': 'Bewegungssteuerung', 'volume': 'Lautstärke', 'mute_unfocused': 'Im Hintergrund stumm',
           'on': 'An', 'off': 'Aus', 'window': 'Fenster', 'centre': 'Mitte', 'edges': 'Am Rand', 'unlimited': 'Unbegrenzt',
           'auto': 'Automatisch', 'keyboard': 'Tastatur', 'player1': 'Spieler 1', 'player2': 'Spieler 2'},
    'ru': {'tag': 'ПОРТ ДЛЯ ПК', 'sub': 'Ваш собственный дамп, нативно на ПК.', 'play': 'ИГРАТЬ', 'running': 'ЗАПУЩЕНО…',
           'install': 'УСТАНОВИТЬ', 'status': 'СОСТОЯНИЕ', 'exe': 'Игра готова', 'noexe': 'Ещё не установлено',
           'dlss': 'Дополнение DLSS найдено', 'nodlss': 'Нет дополнения DLSS', 'cache': 'Кэш шейдеров готов',
           'nocache': 'Шейдеры соберутся при первом запуске', 'folder': 'Открыть папку игры', 'logs': 'Открыть журнал',
           'reinstall': 'Переустановить', 'saved': 'Сохранено',
           'foot': 'Изменения вступят в силу при следующем запуске. Остальное в игре по F1.',
           'display': 'Экран', 'graphics': 'Графика', 'controls': 'Управление', 'audio': 'Звук',
           'fullscreen': 'Полный экран', 'ultrawide': 'Сверхширокий', 'hud_position': 'Положение HUD (сверхширокий)',
           'fps_limit': 'Ограничение кадров', 'upscaler': 'Масштабирование', 'frame_gen': 'Генерация кадров',
           'pad_layout': 'Раскладка кнопок', 'button_glyphs': 'Символы кнопок', 'keyboard_player': 'Клавиатура играет за',
           'motion': 'Управление движением', 'volume': 'Громкость', 'mute_unfocused': 'Без звука в фоне',
           'on': 'Вкл', 'off': 'Выкл', 'window': 'Окно', 'centre': 'Центр', 'edges': 'По краям', 'unlimited': 'Без ограничения',
           'auto': 'Автоматически', 'keyboard': 'Клавиатура', 'player1': 'Игрок 1', 'player2': 'Игрок 2'},
}


# ---- dkb.ini: read and set single keys, keeping every other line ---------------------------------------------------
def ini_read(path):
    vals = {}
    if path.is_file():
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            m = re.match(r'^\s*(?:totk_)?([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$', line)
            if m and not line.lstrip().startswith(('#', ';')):
                vals[m.group(1).lower()] = m.group(2)
    return vals


def ini_set(path, key, value, section):
    lines = path.read_text(encoding='utf-8', errors='replace').splitlines() if path.is_file() else []
    active = re.compile(r'^\s*(?:totk_)?' + re.escape(key) + r'\s*=', re.IGNORECASE)
    commented = re.compile(r'^\s*[#;]\s*(?:totk_)?' + re.escape(key) + r'\s*=', re.IGNORECASE)
    new = f'{key} = {value}'
    for pat in (active, commented):
        for i, line in enumerate(lines):
            if pat.match(line):
                lines[i] = new
                break
        else:
            continue
        break
    else:
        head = next((i for i, line in enumerate(lines) if line.strip().lower() == f'[{section}]'), None)
        if head is None:
            lines += ['', f'[{section}]', new]
        else:
            lines.insert(head + 1, new)
    tmp = path.with_suffix('.ini.tmp')
    tmp.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    os.replace(tmp, path)


def default_dir():
    built = ROOT / 'build' / 'kar458752'
    return built if (built / 'kar.exe').is_file() else Path.cwd()


def system_lang():
    try:
        import ctypes
        lid = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
        return {0x07: 'de', 0x19: 'ru'}.get(lid, 'en')
    except Exception:
        return 'en'


# ---- the window ----------------------------------------------------------------------------------------------------
class Launcher:
    W, H = 1100, 680

    def __init__(self, root, folder, lang):
        self.root, self.dir, self.t = root, folder, TEXT.get(lang, TEXT['en'])
        self.ini = folder / 'dkb.ini'
        self.vals = {**DEFAULTS, **ini_read(self.ini)}
        self.tab = 'graphics'
        self.proc = None
        self.saved_note = False
        root.title('Kirby Air Riders Recomp')
        root.resizable(False, False)
        self.c = tk.Canvas(root, width=self.W, height=self.H, bg=GROUND, highlightthickness=0)
        self.c.pack()
        root.bind('<Return>', lambda e: self.play())
        root.protocol('WM_DELETE_WINDOW', root.destroy)
        self.draw()

    # drawing helpers
    def rrect(self, x0, y0, x1, y1, r, **kw):
        pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1, x1 - r, y1, x0 + r, y1, x0, y1,
               x0, y1 - r, x0, y0 + r, x0, y0]
        return self.c.create_polygon(pts, smooth=True, **kw)

    def star(self, cx, cy, r, fill, outline=''):
        import math
        pts = []
        for i in range(10):
            a = -math.pi / 2 + i * math.pi / 5
            rr = r if i % 2 == 0 else r * 0.45
            pts += [cx + rr * math.cos(a), cy + rr * math.sin(a)]
        self.c.create_polygon(pts, fill=fill, outline=outline, width=2 if outline else 0, joinstyle='round')

    def text(self, x, y, s, size, weight='normal', fill=INK, anchor='nw', **kw):
        return self.c.create_text(x, y, text=s, font=(FONT, size, weight), fill=fill, anchor=anchor, **kw)

    def button(self, x0, y0, x1, y1, label, command, selected=False, size=11, r=12):
        tag = f'b{id(command)}_{x0}_{y0}'
        fill, fg, outline = (ACCENT, CARD, ACCENT) if selected else (CARD, INK, LINE)
        self.rrect(x0, y0, x1, y1, r, fill=fill, outline=outline, width=2, tags=tag)
        self.text((x0 + x1) / 2, (y0 + y1) / 2, label, size, 'bold', fg, anchor='center', tags=tag)
        self.c.tag_bind(tag, '<Button-1>', lambda e: command())
        self.c.tag_bind(tag, '<Enter>', lambda e: self.c.config(cursor='hand2'))
        self.c.tag_bind(tag, '<Leave>', lambda e: self.c.config(cursor=''))

    def label(self, id):
        return self.t.get(id, id)

    # state
    def installed(self):
        return (self.dir / 'kar.exe').is_file()

    def has_dlss(self):
        return (self.dir / 'kar_upscaler.dll').is_file()

    def has_cache(self):
        return (self.dir / 'pipeline-keys.bin').is_file()

    # pages
    def draw(self):
        c, t = self.c, self.t
        c.delete('all')
        c.create_oval(880, -140, 1300, 280, fill=CIRCLE, outline='')
        c.create_oval(-100, 560, 220, 860, fill=CIRCLE, outline='')
        self.star(110, 70, 18, STAR)
        self.star(1040, 600, 14, CARD)
        self.star(1080, 30, 10, STAR)
        # left column
        self.rrect(56, 52, 56 + 12 * len(t['tag']) + 36, 84, 16, fill=INK, outline='')
        self.text(74, 58, t['tag'], 11, 'bold', CARD)
        self.text(52, 100, 'Kirby Air Riders', 34, 'bold', INK)
        self.text(52, 150, 'Recomp', 34, 'bold', ACCENT)
        self.text(56, 222, t['sub'], 13, 'normal', INK_SOFT)
        running = self.proc is not None and self.proc.poll() is None
        big = t['running'] if running else (t['play'] if self.installed() else t['install'])
        tag = 'play'
        c.create_polygon(66, 300, 396, 300, 386, 380, 56, 380, fill=ACCENT_DARK, outline='', tags=tag)
        c.create_polygon(66, 288, 396, 288, 386, 368, 56, 368, fill=MUTED if running else ACCENT, outline='', tags=tag)
        self.star(110, 328, 20, STAR, CARD)
        self.text(240, 328, big, 30, 'bold', CARD, anchor='center', tags=tag)
        if not running:
            c.tag_bind(tag, '<Button-1>', lambda e: self.play())
            c.tag_bind(tag, '<Enter>', lambda e: c.config(cursor='hand2'))
            c.tag_bind(tag, '<Leave>', lambda e: c.config(cursor=''))
        self.text(56, 414, t['status'], 10, 'bold', MUTED)
        chips = [(self.installed(), t['exe'] if self.installed() else t['noexe']),
                 (self.has_dlss(), t['dlss'] if self.has_dlss() else t['nodlss']),
                 (self.has_cache(), t['cache'] if self.has_cache() else t['nocache'])]
        y = 440
        for ok, s in chips:
            self.rrect(56, y, 400, y + 38, 12, fill=CARD, outline='')
            c.create_oval(72, y + 14, 82, y + 24, fill=OK if ok else WARN, outline='')
            self.text(92, y + 19, s, 11, 'bold', INK, anchor='w')
            y += 48
        links = [(t['folder'], self.open_folder), (t['logs'], self.open_log), (t['reinstall'], self.reinstall)]
        x = 56
        for s, cmd in links:
            tid = self.text(x, 618, s, 11, 'bold', ACCENT_DARK)
            c.tag_bind(tid, '<Button-1>', lambda e, f=cmd: f())
            x = c.bbox(tid)[2] + 22
        # settings card
        X0, Y0, X1, Y1 = 450, 52, 1050, 628
        self.rrect(X0 + 4, Y0 + 8, X1 + 4, Y1 + 8, 28, fill='#f2bccf', outline='')
        self.rrect(X0, Y0, X1, Y1, 28, fill=CARD, outline='')
        x = X0 + 18
        for tab_id, _ in TABS:
            w = 18 + 9 * len(self.label(tab_id))
            self.button(x, Y0 + 18, x + w, Y0 + 56, self.label(tab_id), lambda i=tab_id: self.pick_tab(i),
                        selected=self.tab == tab_id, size=12, r=14)
            x += w + 8
        c.create_line(X0 + 18, Y0 + 70, X1 - 18, Y0 + 70, fill='#fde3ee', width=4, capstyle='round')
        y = Y0 + 92
        for key, _cfg, section, opts in dict(TABS)[self.tab]:
            self.text(X0 + 30, y, self.label(key), 13, 'bold', INK)
            y += 30
            x = X0 + 30
            for value, lab in opts:
                if (key, value) in DLSS_VALUES and not self.has_dlss():
                    continue
                s = self.label(lab)
                w = 26 + 8 * len(s)
                if x + w > X1 - 24:
                    x, y = X0 + 30, y + 50
                self.button(x, y, x + w, y + 42, s, lambda k=key, v=value, sec=section: self.set(k, v, sec),
                            selected=self.vals.get(key) == value)
                x += w + 8
            y += 62
        c.create_rectangle(X0, Y1 - 52, X1, Y1 - 26, fill=TINT, outline='')
        self.rrect(X0, Y1 - 52, X1, Y1, 28, fill=TINT, outline='')
        self.text(X0 + 30, Y1 - 26, t['foot'], 10, 'normal', INK_SOFT, anchor='w')
        if self.saved_note:
            self.text(X1 - 30, Y1 - 26, t['saved'], 10, 'bold', OK, anchor='e')

    # actions
    def pick_tab(self, tab_id):
        self.tab = tab_id
        self.draw()

    def set(self, key, value, section):
        if self.proc is not None and self.proc.poll() is None:
            return   # the game's overlay writes the same file while it runs
        ini_set(self.ini, key, value, section)
        self.vals[key] = value
        self.saved_note = True
        self.draw()

    def play(self):
        if self.proc is not None and self.proc.poll() is None:
            return
        if not self.installed():
            self.reinstall()
            return
        self.proc = subprocess.Popen([str(self.dir / 'kar.exe')], cwd=str(self.dir))
        self.draw()
        self.watch()

    def watch(self):
        if self.proc is not None and self.proc.poll() is None:
            self.root.after(1000, self.watch)
            return
        self.vals = {**DEFAULTS, **ini_read(self.ini)}   # the overlay may have changed settings
        self.draw()

    def open_folder(self):
        if os.name == 'nt':
            os.startfile(str(self.dir))

    def open_log(self):
        logs = sorted(self.dir.glob('*.log'), key=lambda p: p.stat().st_mtime)
        if logs and os.name == 'nt':
            os.startfile(str(logs[-1]))

    def reinstall(self):
        subprocess.Popen([sys.executable, '-m', 'installer'], cwd=str(ROOT))


# ---- self-test: every key and value is one runtime/cfg.c accepts ------------------------------------------------------
def selftest():
    src = (ROOT / 'runtime' / 'cfg.c').read_text(encoding='utf-8')
    table = {m.group(1): m.group(2) for m in re.finditer(r'\{"([A-Z0-9_]+)", "[^"]*", "([^"]*)"', src)}
    bad = []
    for _tab, rows in TABS:
        for key, name, _section, opts in rows:
            if key != name.lower():
                bad.append(f'{key}: not the ini spelling of {name}')
            allowed = table.get(name)
            if allowed is None:
                bad.append(f'{name}: not in cfg.c')
                continue
            for value, _label in opts:
                if '..' in allowed:
                    lo, hi = (int(x) for x in allowed.split('..'))
                    ok = value.isdigit() and lo <= int(value) <= hi
                else:
                    ok = value in allowed.split('|')
                if not ok:
                    bad.append(f'{name}={value}: cfg.c allows {allowed}')
    for lang, words in TEXT.items():
        missing = [k for k in TEXT['en'] if k not in words]
        if missing:
            bad.append(f'{lang}: no text for {missing}')
    print('\n'.join(bad) if bad else 'launcher selftest: all keys and values known')
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--dir', type=Path)
    ap.add_argument('--lang', choices=sorted(TEXT))
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if os.name == 'nt':
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
    root = tk.Tk()
    Launcher(root, (args.dir or default_dir()).resolve(), args.lang or system_lang())
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
