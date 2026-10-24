#!/usr/bin/env python3
"""Разбор кадров экрана счётчика М2М из лога ESPHome (log_frames: true).

    python3 tools/frames.py лог.txt            # картинка и распознанный текст каждого нового кадра
    python3 tools/frames.py лог.txt --unknown  # только незнакомые знаки 5x7 — для добавления в font5x7.h

Шрифт берётся из components/m2m_display/font5x7.h, поэтому скрипт распознаёт ровно то же, что и ESP.
"""
import argparse
import os
import re
import sys

W, H = 160, 32
HERE = os.path.dirname(os.path.abspath(__file__))
FONT_H = os.path.join(HERE, "..", "components", "m2m_display", "font5x7.h")


def load_font(path=FONT_H):
    src = open(path, encoding="utf-8").read()
    font = {}
    for ch, w, cols in re.findall(r'\{"((?:[^"\\]|\\.)*)",\s*(\d+),\s*\{([^}]*)\}\}', src):
        w = int(w)
        font[tuple(int(x, 16) for x in cols.split(","))[:w]] = ch.replace('\\"', '"').replace("\\\\", "\\")
    return font


def read_frames(path):
    """Кадры из лога: строки «F00 …» … «F09 …» между FRAME #n BEGIN и FRAME END."""
    frames, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        if "FRAME #" in line and "BEGIN" in line:
            cur = ""
        elif "FRAME END" in line:
            if cur is not None and len(cur) == 1280:
                frames.append(bytes.fromhex(cur))
            cur = None
        elif cur is not None:
            m = re.search(r"(?:\]: |^)F\d\d ([0-9A-F]+)\s*$", line)
            if m:
                cur += m.group(1)
    return frames


def px(f, x, y):
    return (f[(y >> 3) * W + x] >> (y & 7)) & 1


def components(f):
    lab = [[0] * W for _ in range(H)]
    comps = []
    for y in range(H):
        for x in range(W):
            if not px(f, x, y) or lab[y][x]:
                continue
            n = len(comps) + 1
            st, c = [(x, y)], [x, x, y, y]
            lab[y][x] = n
            while st:
                a, b = st.pop()
                c = [min(c[0], a), max(c[1], a), min(c[2], b), max(c[3], b)]
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        u, v = a + dx, b + dy
                        if 0 <= u < W and 0 <= v < H and not lab[v][u] and px(f, u, v):
                            lab[v][u] = n
                            st.append((u, v))
            comps.append(c)
    return lab, comps


def segment(cols, font, unknown):
    text, x, last = "", 0, None
    while x < len(cols):
        if not cols[x]:
            x += 1
            continue
        s = x
        while x < len(cols) and cols[x]:
            x += 1
        g = tuple(cols[s:x])
        ch = font.get(g)
        if ch is None:
            unknown.append(g)
            ch = "?"
        if last is not None and s - last > 2:
            text += " "
        text += ch
        last = x
    return text


def decode(f, font):
    """Заголовок и строки текста — так же, как decoder.h."""
    unknown = []
    title = segment([f[x] & 0x7F for x in range(136)], font, unknown)
    lab, comps = components(f)
    small = sorted([i for i, c in enumerate(comps) if c[2] >= 8 and c[3] - c[2] + 1 <= 8], key=lambda i: comps[i][0])
    lines = []
    for i in small:
        c, best = comps[i], None
        for L in lines:
            if c[2] <= L[3] and c[3] >= L[2] and c[0] - L[1] <= 8:
                best = L
        if best:
            best[1], best[2], best[3] = max(best[1], c[1]), min(best[2], c[2]), max(best[3], c[3])
            best[4].append(i)
        else:
            lines.append([c[0], c[1], c[2], c[3], [i]])
    out = []
    for L in lines:
        ys = [comps[i][3] for i in L[4]]
        top = max(set(ys), key=ys.count) - 6
        ids = {j + 1 for j in L[4]}
        cols = [sum(1 << r for r in range(7) if 0 <= top + r < H and lab[top + r][x] in ids)
                for x in range(L[0], L[1] + 1)]
        unk = []
        t = segment(cols, font, unk)
        if t.replace("?", "").replace(" ", ""):
            out.append((top, L[0], t))
            unknown += unk
    return title, sorted(out), unknown


def picture(f):
    return "\n".join("".join("#" if px(f, x, y) else "." for x in range(W)) for y in range(H))


def glyph_art(g):
    return "\n".join("    " + "".join("#" if (c >> r) & 1 else "." for c in g) for r in range(7))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--unknown", action="store_true", help="только незнакомые знаки")
    a = ap.parse_args()
    font = load_font()
    frames = read_frames(a.log)
    if not frames:
        sys.exit("В логе нет кадров. Включите log_frames: true.")
    seen, unk_seen = set(), {}
    for n, f in enumerate(frames):
        title, lines, unknown = decode(f, font)
        for g in unknown:
            unk_seen.setdefault(g, title)
        if a.unknown or f in seen:
            continue
        seen.add(f)
        print(f"=== кадр {n}: «{title}»")
        for top, x0, t in lines:
            print(f"    строка y={top} x={x0}: {t}")
        print(picture(f))
        print()
    if unk_seen:
        print(f"Незнакомых знаков: {len(unk_seen)}")
        for g, title in unk_seen.items():
            print(f'  {{"?", {len(g)}, {{{", ".join("0x%02X" % c for c in g)}}}}},   // экран «{title}»')
            print(glyph_art(g))


if __name__ == "__main__":
    main()
