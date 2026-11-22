#!/usr/bin/env python3
"""drive.py - run one regression game in Hatari (a plain-ST reference) with
an input script, and take screenshots, so the scripts the PiStorm runs can
be worked out and checked off the hardware.

    HATARI=/path/to/patched/hatari python3 drive.py SCRIPT OUTDIR

SCRIPT (tools/regress/games/<game>.in) holds the setup lines

    start D:\\JIMPOWER\\START.TOS       program to autostart
    seconds 60                         how long to run after it starts
    shots 2                            screenshot every N seconds (0 = none)

and the input, one event a line, timed in seconds from the moment the
program is started (Pexec):

    3.0  fire              joystick 1 fire: pressed for 0.2 s
    3.0  joy1 right fire   joystick 1 held like this until the next joy1 line
    3.5  joy1 -            joystick 1 let go
    4.0  key 0x39          ST scancode pressed for 0.1 s (0x39 Space)
    4.0  keydown 0x1D      held ...
    4.5  keyup 0x1D        ... until let go
    20   mark play         a label: the summary says what was on screen

Needs Hatari 2.4.1 with hatari-2.4.1-joy.patch (the control socket's
"joy <port> <bits> <0|1>" event: Hatari has no other way to press a
joystick from outside). Runs at real speed, so script seconds are guest
seconds. The disk is a copy of the regression disk (RG_IMG, default
regress-games.img beside this script: dkimages' word-swapped image) with
RUN.INF set to the script's start, booted as on the PiStorm - C:\AUTO\
LAUNCH.PRG starts the game - and time 0 is that program's Pexec (Hatari's
GEMDOS trace). Machine: plain ST, TOS 2.06, 4 MB (tos.rom beside this).
"""
import os, socket, subprocess, sys, time, threading, re

JOY = {'up': 1, 'down': 2, 'left': 4, 'right': 8, 'fire': 0x80}


def load(path):
    setup, ev = {'seconds': '60', 'shots': '2'}, []
    for ln in open(path):
        ln = ln.split('#', 1)[0].strip()
        if not ln:
            continue
        p = ln.split()
        if p[0] in ('start', 'seconds', 'shots', 'machine'):
            setup[p[0]] = ln.split(None, 1)[1]
            continue
        t = float(p[0])
        if p[1] == 'fire':
            ev += [(t, 'joy', 0x80), (t + 0.2, 'joy', 0)]
        elif p[1] == 'joy1':
            bits = 0
            for w in p[2:]:
                if w != '-':
                    bits |= JOY[w]
            ev.append((t, 'joy', bits))
        elif p[1] == 'key':
            k = int(p[2], 0)
            ev += [(t, 'kd', k), (t + 0.1, 'ku', k)]
        elif p[1] == 'keydown':
            ev.append((t, 'kd', int(p[2], 0)))
        elif p[1] == 'keyup':
            ev.append((t, 'ku', int(p[2], 0)))
        elif p[1] == 'mark':
            ev.append((t, 'mark', ' '.join(p[2:])))
        else:
            sys.exit('%s: unknown event %s' % (path, ln))
    ev.sort(key=lambda e: e[0])
    return setup, ev


def main():
    script, outdir = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    setup, ev = load(script)
    os.makedirs(outdir, exist_ok=True)
    # the disk exactly as the PiStorm boots it: a copy of the regression
    # disk with RUN.INF naming this game (LAUNCH starts it from C:\AUTO),
    # unswapped for Hatari's ACSI
    src = os.environ.get('RG_IMG', os.path.join(here, 'regress-games.img'))
    img = os.path.join(outdir, 'disk.img')
    raw = open(src, 'rb').read()
    open(img, 'wb').write(raw)
    subprocess.run([sys.executable, os.path.join(here, '..', 'games-img', 'mkgamesimg.py'),
                    '--set-start', img, setup['start']], check=True, stdout=subprocess.DEVNULL)
    raw = open(img, 'rb').read()
    sw = bytearray(len(raw))
    sw[0::2], sw[1::2] = raw[1::2], raw[0::2]
    open(img, 'wb').write(sw)
    want = setup['start'].rsplit('\\', 1)[-1].upper()
    sock = '/tmp/hatari-drive-%d.sock' % os.getpid()
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock)
    srv.listen(1)
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    args = [os.environ.get('HATARI', 'hatari'), '--confirm-quit', 'no',
            '--machine', setup.get('machine', 'st'), '--tos', os.path.join(here, 'tos.rom'),
            '--memsize', '4', '--cpulevel', '0', '--cpuclock', '8', '--compatible', 'on',
            '--cpu-exact', 'on', '--blitter', 'off', '--acsi', '0=' + img, '--trace', 'gemdos',
            '--sound', 'off', '--natfeats', 'off',
            '--joy0', 'none', '--joy1', 'keys', '--control-socket', sock,
            '--screenshot-dir', outdir,
            '--log-file', os.path.join(outdir, 'hatari.log')]
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True)
    c, _ = srv.accept()
    started = threading.Event()
    log = open(os.path.join(outdir, 'out.txt'), 'w')

    def reader():
        for ln in p.stdout:
            log.write(ln)
            if 'Pexec' in ln and want in ln.upper():
                started.set()
    threading.Thread(target=reader, daemon=True).start()
    if not started.wait(120):
        p.kill()
        sys.exit('the program never started (no Pexec in 120 s)')
    t0 = time.time()
    end = float(setup['seconds'])
    shot = float(setup['shots'])
    next_shot = 0.0
    marks = []
    i = 0
    nshot = 0
    while time.time() - t0 < end:
        now = time.time() - t0
        while i < len(ev) and ev[i][0] <= now:
            t, k, v = ev[i]
            if k == 'joy':
                for b in (1, 2, 4, 8, 0x80):
                    c.send(b'hatari-event joy 1 %d %d\n' % (b, 1 if v & b else 0))
            elif k == 'kd':
                c.send(b'hatari-event keydown %d\n' % v)
            elif k == 'ku':
                c.send(b'hatari-event keyup %d\n' % v)
            elif k == 'mark':
                marks.append((t, v, nshot + 1))
            i += 1
        if shot and now >= next_shot:
            c.send(b'hatari-shortcut screenshot\n')
            nshot += 1
            next_shot += shot
        time.sleep(0.02)
    c.send(b'hatari-shortcut quit\n')
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
    with open(os.path.join(outdir, 'summary.txt'), 'w') as f:
        f.write('%s: %d screenshots every %gs from the start\n' % (script, nshot, shot))
        for t, v, n in marks:
            f.write('mark %s at %.1fs: grab%04d.png\n' % (v, t, n))
    print(open(os.path.join(outdir, 'summary.txt')).read(), end='')


if __name__ == '__main__':
    main()
