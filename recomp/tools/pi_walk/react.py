# Plays the board part of the setup like a person: watches the dialog box and steps
# on or off when the game asks. Anything it doesn't know gets A every 6 seconds and is
# saved once as u-NN.png, so new screens can be added to ref/.
# usage: react.py <start-epoch> <seconds-left-in-run>
import os, subprocess, sys, time
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
T0, END = float(sys.argv[1]), time.time() + float(sys.argv[2]) - 1
BOX = (60, 186, 560, 240)
# Screens where A only works on a button: where to point first.
CLICKS = {'clothing': (0.575, 0.2)}
# Where buttons usually sit, tried in turn when a screen nobody knows stops changing:
# Next and OK at the bottom right, the middle, then the bottom left.
TRY = [(0.33, 0.68), (0.6, 0.76), (0.34, 0.78), (0, 0.6), (-0.33, 0.68), (0, 0.2)]
STILL = 15
refs = {}
for f in sorted(os.listdir(os.path.join(HERE, 'ref'))):
    name, _ = os.path.splitext(f)
    refs[name] = np.asarray(Image.open(os.path.join(HERE, 'ref', f)).convert('L'), dtype=np.float32)
log = open(os.path.join(HERE, 'react.log'), 'w', buffering=1)

def send(line):
    with open(os.path.join(HERE, 'control'), 'w') as pipe:
        pipe.write(line + '\n')
    log.write('%5.0f   -> %s\n' % (time.time() - T0, line))

board, off_at, last_a, last_shot, shots = True, 0, 0, 0, 0
unknown_seen, last_name = [], None
still_since, last_small, tries = 0, None, 0
shot = os.path.join(HERE, 'r.png')
while time.time() < END:
    tick = time.time()
    if subprocess.run(['grim', '-s', '0.5', shot]).returncode:
        time.sleep(1); continue
    image = Image.open(shot)
    crop = np.asarray(image.convert('L').crop(BOX), dtype=np.float32)
    scores = {k: float(np.abs(crop - r).mean()) for k, r in refs.items()}
    best = min(scores, key=scores.get)
    name = best.split('-')[0] if scores[best] < 5 else 'unknown'
    now = time.time()
    # A screen that has stopped changing, Mii blinks aside, is waiting on a button.
    small = np.asarray(image.convert('L').resize((160, 90)), dtype=np.float32)
    if last_small is None or np.abs(small - last_small).mean() >= 3:
        still_since, last_small = now, small
    # Today's square on the calendar is a large pale yellow block.
    rgb = np.asarray(image.convert('RGB').resize((160, 90)), dtype=np.int16)
    yellow = (rgb[:, :, 0] > 235) & (rgb[:, :, 1] > 230) & (rgb[:, :, 2] > 150) & (rgb[:, :, 2] < 205)
    stamp = None
    if 0.1 < yellow.mean() < 0.35:
        ys, xs = np.nonzero(yellow)
        stamp = ((xs.mean() / 160 - 0.5) * 2, (ys.mean() / 90 - 0.5) / 0.55)
    if name != last_name:
        log.write('%5.0f %s (%.1f)\n' % (now - T0, name, scores[best]))
        last_name = name
    if name == 'stepon':
        if not board:
            send('board on 60 0 0'); board = True
    elif name in ('cantfind', 'reboot', 'stepoff'):
        if board:
            send('board off'); board, off_at = False, now
        elif now - off_at >= 2 and now - last_a >= 4:
            send('press a'); last_a = now
    elif name in CLICKS:
        if now - last_a >= 4:
            x, y = CLICKS[name]
            send('pointer %g %g' % (x, y)); time.sleep(0.5); send('press a'); time.sleep(0.5); send('pointer 0 0.6')
            last_a = time.time()
    elif name == 'unknown' and stamp is not None:
        # The calendar: the stamp follows the pointer and lags it, so it's held over
        # today's square for a moment before A, and left there while it stamps. Nothing
        # else is tried here, or it would keep pulling the stamp away.
        if now - last_a >= 6:
            send('pointer %g %g' % stamp); time.sleep(2); send('press a'); time.sleep(3)
            log.write('%5.0f stamped at %.2f %.2f\n' % (now - T0, stamp[0], stamp[1]))
            last_a = time.time()
    elif name == 'unknown' and now - still_since >= STILL and now - last_a >= 4:
        x, y = TRY[tries % len(TRY)]; tries += 1
        send('pointer %g %g' % (x, y)); time.sleep(0.5); send('press a'); time.sleep(0.5); send('pointer 0 0.6')
        last_a = time.time()
    elif now - last_a >= 6:
        send('press a'); last_a = now
    if name == 'unknown' and all(np.abs(crop - u).mean() >= 5 for u in unknown_seen):
        unknown_seen.append(crop)
        image.save(os.path.join(HERE, 'u-%02d.png' % len(unknown_seen)))
        log.write('%5.0f saved u-%02d\n' % (now - T0, len(unknown_seen)))
    if now - last_shot >= 10:
        image.save(os.path.join(HERE, 'w-%02d.png' % shots)); shots += 1; last_shot = now
    time.sleep(max(0, 1 - (time.time() - tick)))
