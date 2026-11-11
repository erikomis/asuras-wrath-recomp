#!/usr/bin/env python3
"""Closed-loop pad driver for the Asura's Wrath smoke test.

A timed PAD_SCRIPT cannot walk the title menus reliably: with a save that has
progress the cursor starts on EXTRAS or EPISODE MENU depending on boot timing.
This driver looks at the probe's frame dump, works out which screen the game is
on, and writes the next button into the PAD_FILE that cellPad already reads
(see libs/input/cellPad.c, "PAD_FILE=<path> -- closed-loop input"). It changes
nothing in the game or the runtime: the file is the only channel.

    tools/pad_driver.py <frame.ppm> <pad_file> <seconds> <log>
    tools/pad_driver.py --classify <frame.ppm> [...]     # print what it sees

Run it next to a probe (docs/SMOKE_TEST_EPISODIO1.md has the exact commands):
the probe writes cache/probe/<label>.ppm every PS3RECOMP_METAL_FRAME_DUMP_EVERY
presents, the driver reads it and answers through the pad file.

Screens it recognises (1280x720 coordinates):
  * a red selection bar: title rows at y~393/428/463/498 (NEW GAME, EPISODE
    MENU, OPTIONS, EXTRAS), the PRESS START bar at y~475, the difficulty list
    (EASY at y~348) and the confirm dialog (Yes ~390, No ~431);
  * two bright rules at y~242 and y~477: a dialog is on screen. A dialog with
    no red bar is a message ("Load successful", "Save successful").

Sequence it performs once per run:
  START at PRESS START -> CROSS on "Load successful" -> UP to NEW GAME ->
  CROSS -> CROSS on EASY -> UP to "Yes" -> CROSS -> CROSS on "Save successful"
  -> (about 70 s of loading ends on the episode's PRESS START card) START once
  -> stop. After that it injects nothing: gameplay frames are mostly red and
  would be taken for a menu.
"""
import os
import re
import shutil
import sys
import time

UP, CROSS, START = "0x0010 4", "0x4000 4", "0x0008 4"   # mask + number of reports


def load(path):
    """Return (bytes, width, height, offset) of a complete P6 PPM, else None."""
    with open(path, "rb") as f:
        d = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d)
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    off = m.end()
    if len(d) < off + w * h * 3:          # still being written by the game
        return None
    return d, w, h, off


def features(path):
    r = load(path)
    if not r:
        return None
    d, w, h, off = r
    sx, sy = w / 1280.0, h / 720.0

    def px(x, y):
        i = off + (int(y * sy) * w + int(x * sx)) * 3
        return d[i], d[i + 1], d[i + 2]

    # Red selection bar: fraction of red pixels per row, across x 400..880.
    bar = []
    for y in range(330, 520, 2):
        n = t = 0
        for x in range(400, 880, 10):
            R, G, B = px(x, y)
            t += 1
            if R > 110 and G < 50 and B < 40 and R > 3 * G:
                n += 1
        bar.append((y, n / t))
    ys = [y for y, f in bar if f > 0.35]
    bar_y = (sum(ys) / len(ys)) if ys else None

    # The two thin bright rules of a dialog, y~242 and y~477, full width.
    def line_frac(y):
        n = t = 0
        for x in range(60, 1220, 20):
            R, G, B = px(x, y)
            t += 1
            if (R + G + B) / 3 > 150:
                n += 1
        return n / t

    dialog = (max(line_frac(y) for y in (241, 242, 243)) > 0.6 and
              max(line_frac(y) for y in (476, 477, 478)) > 0.6)

    # The difficulty list has "EASY" written at y~348 (72 bright pixels measured
    # there); the Yes/No confirmation has nothing on that row (0). Both dialogs
    # have a red bar at y~390 or ~431, so the bar alone cannot tell them apart.
    text = 0
    for y in range(338, 360, 2):
        for x in range(560, 720, 2):
            R, G, B = px(x, y)
            if (R + G + B) / 3 > 170:
                text += 1
    return {"bar_y": bar_y, "dialog": dialog, "list3": text > 20}


def title_row(y):
    for name, c in (("NEW GAME", 393), ("EPISODE MENU", 428),
                    ("OPTIONS", 463), ("EXTRAS", 498)):
        if abs(y - c) <= 8:
            return name
    return None


def drive(frame, padf, secs, logp):
    log = open(logp, "a")
    t0 = time.time()
    st = {"last_press": 0.0, "stage": 0, "prev": None, "same": 0, "presses": 0}

    def say(m):
        log.write(f"[{time.time() - t0:6.1f}s] {m}\n")
        log.flush()

    def press(btn, why):
        with open(padf, "w") as f:
            f.write(btn + "\n")
        st["last_press"] = time.time()
        st["presses"] += 1
        say(f"PRESS {btn.split()[0]} ({why})")

    say("start")
    while time.time() - t0 < secs:
        time.sleep(0.5)
        if os.path.exists(padf) and os.path.getsize(padf) > 0:
            continue                       # the game has not consumed it yet
        try:
            f = features(frame)
        except Exception:
            f = None
        if not f:
            continue
        key = (f["dialog"], None if f["bar_y"] is None else round(f["bar_y"] / 6))
        st["same"] = st["same"] + 1 if key == st["prev"] else 0
        st["prev"] = key
        # Wait for a stable screen and at least 5 s since the last press.
        if st["same"] < 2 or time.time() - st["last_press"] < 5:
            continue
        by, dlg = f["bar_y"], f["dialog"]
        stage = st["stage"]
        if stage >= 6:                                   # episode started: hands off.
            continue    # Gameplay frames can look like a dialog or a menu; a run once
                        # sent a CROSS from inside a QTE for that reason.

        if dlg and by is None:                           # a plain message
            press(CROSS, "close message")
            if stage >= 4:
                st["stage"] = 5
                say("gameplay mode: no more keys except the episode START card")
            continue
        if stage >= 5:
            if stage == 5 and by is not None and 465 <= by <= 485 and not dlg:
                st["stage"] = 6
                press(START, "episode PRESS START card (once)")
                say("gameplay: injecting nothing more")
            continue
        if dlg and by is not None:                       # difficulty / confirm
            if f["list3"]:                               # EASY / NORMAL / HARD
                if by < 360:
                    st["stage"] = max(stage, 2)
                    press(CROSS, f"difficulty EASY (bar {by:.0f})")
                else:
                    press(UP, f"difficulty: up to EASY (bar {by:.0f})")
            else:                                        # "Start the game on EASY?"
                st["stage"] = max(stage, 3)
                say(f"confirm dialog, bar={by:.0f}")
                if by > 410:                             # cursor on "No" (the default)
                    press(UP, "confirm: UP (move cursor to Yes)")
                else:                                    # cursor on "Yes"
                    st["stage"] = 4
                    try:
                        shutil.copy(frame, logp + ".confirm.ppm")
                    except Exception:
                        pass
                    press(CROSS, "confirm: CROSS on Yes")
            continue
        if by is not None and stage < 4:                 # title menu / PRESS START
            r = title_row(by)
            if r is None and 465 <= by <= 485:
                press(START, f"PRESS START (bar {by:.0f})")
            elif r == "NEW GAME":
                press(CROSS, "menu: NEW GAME")
            elif r:
                press(UP, f"menu: cursor on {r}, go up")
            continue
    say(f"end; keys sent={st['presses']}, stage={st['stage']}")
    log.close()


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--classify":
        for p in sys.argv[2:]:
            print(os.path.basename(p), features(p))
    elif len(sys.argv) == 5:
        drive(sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4])
    else:
        print(__doc__)
        sys.exit(2)
