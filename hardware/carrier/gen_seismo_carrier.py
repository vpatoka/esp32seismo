#!/usr/bin/env python3
"""Draw the VSEISMO carrier board schematic and PCB placement as SVG, and write
the KiCad netlist plus Markdown net and footprint tables from one parts list.

Usage: python3 gen_seismo_carrier.py <outdir>

Outputs: seismo-carrier-schematic.svg, seismo-carrier-pcb.svg,
seismo-carrier.net (KiCad netlist, for Pcbnew "Import Netlist"),
seismo.pretty/ and fp-lib-table (the project footprint library the netlist
refers to as "seismo:"), "Seismo Carrier Netlist.md" and "Seismo Carrier
Footprints.md" (Obsidian notes, embedded in the carrier note). The drawings are hand placed; the
netlist comes from PARTS at the bottom. When a connection changes, change
both and regenerate. Drawing helpers are the ones from the Nixie carrier
generator, so both boards read the same way.
"""
import sys, os, uuid, datetime

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

INK = "#111"
NET = "#0645ad"
PWR = "#b00020"
GRY = "#666"
FONT = "font-family='DejaVu Sans Mono, Consolas, monospace'"

# Series resistors on the DevKit-to-Click lines: one table for the drawing
# and the netlist. 330R on the lines that idle high (CS, RST, CS2) limits
# back-power into an unpowered Click to under 10 mA per pin.
SERIES_R = {"R10": "33R", "R11": "33R", "R12": "33R", "R13": "330R", "R14": "33R",
            "R15": "330R", "R16": "330R"}


class Svg:
    def __init__(s, w, h, title):
        s.w, s.h, s.el = w, h, []
        s.el.append(f"<rect x='0' y='0' width='{w}' height='{h}' fill='#fff'/>")
        s.text(20, 34, title, 22, bold=True)

    def line(s, x1, y1, x2, y2, c=INK, w=1.6, dash=None):
        d = f" stroke-dasharray='{dash}'" if dash else ""
        s.el.append(f"<line x1='{x1}' y1='{y1}' x2='{x2}' y2='{y2}' stroke='{c}' stroke-width='{w}'{d}/>")

    def rect(s, x, y, w, h, c=INK, fill="none", sw=1.6, dash=None, rx=0):
        d = f" stroke-dasharray='{dash}'" if dash else ""
        s.el.append(f"<rect x='{x}' y='{y}' width='{w}' height='{h}' rx='{rx}' stroke='{c}' "
                    f"stroke-width='{sw}' fill='{fill}'{d}/>")

    def circle(s, x, y, r, c=INK, fill="none", sw=1.4):
        s.el.append(f"<circle cx='{x}' cy='{y}' r='{r}' stroke='{c}' stroke-width='{sw}' fill='{fill}'/>")

    def text(s, x, y, t, size=13, c=INK, anchor="start", bold=False, rot=None):
        t = t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        b = " font-weight='bold'" if bold else ""
        r = f" transform='rotate({rot} {x} {y})'" if rot is not None else ""
        s.el.append(f"<text x='{x}' y='{y}' font-size='{size}' fill='{c}' text-anchor='{anchor}' "
                    f"{FONT}{b}{r}>{t}</text>")

    def dot(s, x, y):
        s.circle(x, y, 3, INK, INK)

    def save(s, name):
        with open(os.path.join(OUT, name), "w") as f:
            f.write(f"<svg xmlns='http://www.w3.org/2000/svg' width='{s.w}' height='{s.h}' "
                    f"viewBox='0 0 {s.w} {s.h}'>\n")
            f.write("\n".join(s.el))
            f.write("\n</svg>\n")


# ── schematic symbols ─────────────────────────────────────────────────────
def res_h(s, x, y, ref, val, length=80):
    """Horizontal resistor between (x,y) and (x+length,y)."""
    body = 36
    a = x + (length - body) / 2
    s.line(x, y, a, y)
    s.rect(a, y - 7, body, 14)
    s.line(a + body, y, x + length, y)
    s.text(a + body / 2, y - 11, f"{ref} {val}", 11, anchor="middle")


def res_v(s, x, y, ref, val, length=60, dnp=False):
    """Vertical resistor from (x,y) down to (x,y+length)."""
    body = 30
    a = y + (length - body) / 2
    s.line(x, y, x, a)
    s.rect(x - 7, a, 14, body, dash="4 3" if dnp else None)
    s.line(x, a + body, x, y + length)
    s.text(x + 11, a + 12, ref, 11)
    s.text(x + 11, a + 25, val + (" DNP" if dnp else ""), 11)


def gnd(s, x, y):
    s.line(x, y, x, y + 8)
    s.line(x - 10, y + 8, x + 10, y + 8, w=2)
    s.line(x - 6, y + 12, x + 6, y + 12, w=2)
    s.line(x - 2, y + 16, x + 2, y + 16, w=2)


def pwr(s, x, y, name):
    s.line(x, y, x, y - 10, c=PWR)
    s.line(x - 9, y - 10, x + 9, y - 10, c=PWR, w=2)
    s.text(x, y - 15, name, 12, c=PWR, anchor="middle", bold=True)


def netlab(s, x, y, name, side="r"):
    """Net label flag; side='r' text to the right of (x,y)."""
    w = 8 * len(name) + 10
    if side == "r":
        s.rect(x, y - 9, w, 18, c=NET, rx=3)
        s.text(x + 5, y + 4, name, 12, c=NET)
    else:
        s.rect(x - w, y - 9, w, 18, c=NET, rx=3)
        s.text(x - w + 5, y + 4, name, 12, c=NET)


def cap_v(s, x, y, ref, val, polar=False, length=60):
    mid = y + length / 2
    s.line(x, y, x, mid - 4)
    s.line(x - 12, mid - 4, x + 12, mid - 4, w=2.4)
    if polar:
        s.el.append(f"<path d='M {x-12} {mid+6} Q {x} {mid} {x+12} {mid+6}' stroke='{INK}' "
                    f"stroke-width='2.4' fill='none'/>")
        s.text(x - 20, mid - 6, "+", 12)
        s.line(x, mid + 3, x, y + length)
    else:
        s.line(x - 12, mid + 4, x + 12, mid + 4, w=2.4)
        s.line(x, mid + 4, x, y + length)
    s.text(x + 16, mid - 2, ref, 11)
    s.text(x + 16, mid + 11, val, 11)


def diode_h(s, x, y, ref, val, length=80, led=False):
    """Diode, anode left, cathode right."""
    a = x + length / 2 - 10
    s.line(x, y, a, y)
    s.el.append(f"<path d='M {a} {y-9} L {a} {y+9} L {a+18} {y} Z' stroke='{INK}' stroke-width='1.6' "
                f"fill='{'#fff' if led else INK}'/>")
    s.line(a + 18, y - 9, a + 18, y + 9, w=2)
    s.line(a + 18, y, x + length, y)
    if led:
        s.line(a + 6, y - 12, a + 14, y - 20)
        s.line(a + 12, y - 10, a + 20, y - 18)
    s.text(a + 9, y + 24, f"{ref} {val}", 11, anchor="middle")


def diode_v(s, x, y, ref, val, length=70, up=True, led=False):
    """Vertical diode. up=True: anode at bottom, cathode at top."""
    m = y + length / 2
    s.line(x, y, x, m - 9)
    s.line(x, m + 9, x, y + length)
    fill = "#fff" if led else INK
    if up:
        s.el.append(f"<path d='M {x-9} {m+9} L {x+9} {m+9} L {x} {m-9} Z' stroke='{INK}' "
                    f"stroke-width='1.6' fill='{fill}'/>")
        s.line(x - 9, m - 9, x + 9, m - 9, w=2)
    else:
        s.el.append(f"<path d='M {x-9} {m-9} L {x+9} {m-9} L {x} {m+9} Z' stroke='{INK}' "
                    f"stroke-width='1.6' fill='{fill}'/>")
        s.line(x - 9, m + 9, x + 9, m + 9, w=2)
    if led:
        s.line(x + 12, m - 4, x + 20, m - 12)
        s.line(x + 12, m + 3, x + 20, m - 5)
    s.text(x + 14, m + 4 if not led else m + 16, f"{ref}", 11)
    s.text(x + 14, m + 17 if not led else m + 29, val, 11)


def terminal(s, x, y, ref, title, pins, side="r", pitch=26):
    """Screw terminal block; pins listed top-down; wires leave on 'side'."""
    h = pitch * len(pins)
    s.rect(x, y, 34, h, fill="#f4f4f4")
    s.text(x + 17, y - 22, ref, 12, anchor="middle", bold=True)
    s.text(x + 17, y - 8, title, 11, anchor="middle", c=GRY)
    pts = []
    for i, p in enumerate(pins):
        py = y + pitch / 2 + i * pitch
        s.circle(x + 17, py, 7)
        s.line(x + 12, py + 5, x + 22, py - 5, w=1)
        s.text(x + 17 + (0 if side == "r" else 0), py + 4, "", 10)
        if side == "r":
            s.text(x - 6, py + 4, f"{i+1} {p}", 11, anchor="end")
            pts.append((x + 34, py))
        else:
            s.text(x + 40, py + 4, f"{i+1} {p}", 11)
            pts.append((x, py))
    return pts


def ic(s, x, y, ref, part, left, right, w=150, pitch=26):
    """IC box; left/right = lists of (pin_no, name). Returns dict name->(x,y) endpoints."""
    n = max(len(left), len(right))
    h = pitch * n + 20
    s.rect(x, y, w, h, fill="#fbfbf3")
    s.text(x + w / 2, y - 22, ref, 13, anchor="middle", bold=True)
    s.text(x + w / 2, y - 8, part, 11, anchor="middle", c=GRY)
    pins = {}
    for i, (no, nm) in enumerate(left):
        py = y + 20 + i * pitch
        s.line(x - 24, py, x, py)
        s.text(x - 4, py - 4, str(no), 10, anchor="end", c=GRY)
        s.text(x + 6, py + 4, nm, 12)
        pins[nm] = (x - 24, py)
        pins[f"L{no}"] = (x - 24, py)
    for i, (no, nm) in enumerate(right):
        py = y + 20 + i * pitch
        s.line(x + w, py, x + w + 24, py)
        s.text(x + w + 4, py - 4, str(no), 10, c=GRY)
        s.text(x + w - 6, py + 4, nm, 12, anchor="end")
        pins[nm] = (x + w + 24, py)
        pins[f"R{no}"] = (x + w + 24, py)
    return pins, h


def panel(s, x, y, w, h, title):
    s.rect(x, y, w, h, c=GRY, dash="6 4", sw=1.2)
    s.text(x + 10, y + 20, title, 15, bold=True)



# ══ SCHEMATIC ═════════════════════════════════════════════════════════════
S = Svg(1960, 1880, "VSEISMO carrier board, schematic rev A (design reference, not for fabrication)")
S.text(20, 58, "Net-label style: same blue label = same net. Pin numbers in grey. "
       "Power comes from the EQAMP J8 (raw DC), so the power return already joins AMP_GND and GND.", 13, c=GRY)

# ── A: power ──────────────────────────────────────────────────────────────
panel(S, 20, 80, 900, 420, "A. Power: raw DC from EQAMP J8, U1 makes 5 V, U2 makes a clean 3.3 V for the ADC")
p = terminal(S, 60, 150, "P1", "DC IN (J8)", ["+VIN", "GND"])
y12 = p[0][1]
S.line(*p[1], 100, p[1][1]); gnd(S, 100, p[1][1])
S.line(*p[0], 130, y12, c=PWR)
S.rect(130, y12 - 8, 50, 16, fill="#fff")
S.text(155, y12 + 4, "PTC", 10, anchor="middle")
S.text(155, y12 - 13, "F1 RXEF050", 11, anchor="middle")
S.line(180, y12, 200, y12, c=PWR)
diode_h(S, 200, y12, "D3", "1N5819", length=90)
S.line(290, y12, 520, y12, c=PWR)
S.text(320, y12 - 8, "+VIN", 10, c=NET)
S.dot(330, y12); diode_v(S, 330, y12, "D1", "P6KE30A", length=60, up=True); gnd(S, 330, y12 + 60)
S.dot(420, y12); cap_v(S, 420, y12, "C1", "22uF 50V", polar=True, length=60); gnd(S, 420, y12 + 60)
yr = 300
S.line(520, y12, 520, yr, c=PWR); S.line(520, yr, 540, yr, c=PWR)
S.rect(540, yr - 22, 100, 50, fill="#fbfbf3")
S.text(590, yr - 46, "U1", 13, anchor="middle", bold=True)
S.text(590, yr - 31, "OKI-78SR-5/1.5-W36-C", 10, anchor="middle", c=GRY)
S.text(546, yr + 4, "IN", 11); S.text(634, yr + 4, "OUT", 11, anchor="end")
S.text(590, yr + 22, "GND", 11, anchor="middle")
S.line(590, yr + 28, 590, yr + 44); gnd(S, 590, yr + 44)
S.line(640, yr, 880, yr, c=PWR)
pwr(S, 660, yr, "+5V")
S.dot(690, yr); cap_v(S, 690, yr, "C2", "100uF 25V", polar=True, length=60); gnd(S, 690, yr + 60)
S.dot(770, yr); cap_v(S, 770, yr, "C3", "100nF", length=60); gnd(S, 770, yr + 60)
S.dot(840, yr); res_v(S, 840, yr, "R1", "2.2k", length=50)
diode_v(S, 840, yr + 50, "LED1", "green", length=50, up=False, led=True); gnd(S, 840, yr + 100)
# DevKit feed and the analog 3.3 V
ya = 420
pwr(S, 60, ya, "+5V")
diode_h(S, 60, ya, "D2", "1N5817", length=100)
netlab(S, 160, ya, "DK_5V")
pwr(S, 300, ya, "+5V")
S.line(300, ya, 310, ya)
S.rect(310, ya - 8, 50, 16, fill="#ddd")
S.text(335, ya + 22, "FB1 ferrite", 11, anchor="middle")
S.line(360, ya, 450, ya, c=PWR)
S.text(400, ya - 8, "+5V_A", 10, c=NET, anchor="middle")
S.dot(380, ya); cap_v(S, 380, ya, "C4", "10uF", polar=True, length=40); gnd(S, 380, ya + 40)
S.rect(450, ya - 20, 90, 44, fill="#fbfbf3")
S.text(495, ya - 26, "U2 LP2950ACZ-3.3", 11, anchor="middle", bold=True)
S.text(456, ya + 4, "IN 3", 10); S.text(534, ya + 4, "1 OUT", 10, anchor="end")
S.line(495, ya + 24, 495, ya + 34); gnd(S, 495, ya + 34)
S.line(540, ya, 720, ya, c=PWR)
S.dot(580, ya); cap_v(S, 580, ya, "C5", "10uF", polar=True, length=40); gnd(S, 580, ya + 40)
S.dot(640, ya); cap_v(S, 640, ya, "C6", "100nF", length=40); gnd(S, 640, ya + 40)
netlab(S, 720, ya, "+3V3A")
S.text(560, 130, "One adapter (18-24 V DC) feeds the EQAMP. P1 takes", 11, c=GRY)
S.text(560, 144, "its J8 raw DC (after its bridge, before its", 11, c=GRY)
S.text(560, 158, "regulators): about 15-27 V. Never its J9 +-12 V.", 11, c=GRY)
S.text(560, 180, "D1 P6KE33A (28.2 V standoff) stays off across the", 11, c=GRY)
S.text(560, 194, "whole J8 range. D3 is redundant behind the amp", 11, c=GRY)
S.text(560, 208, "bridge but harmless.", 11, c=GRY)
S.text(40, 492, "D2: the DevKit never fights its own USB 5 V.  +3V3A feeds only the Click (AVDD = DVDD there) and Y1.", 11, c=GRY)

# ── B: DevKit ─────────────────────────────────────────────────────────────
panel(S, 940, 80, 1000, 620, "B. ESP32-C5-DevKitC-1 v1.2 (DK1) on two 1x16 female headers")
j1 = [(1, "3V3"), (2, "RST"), (3, "GPIO2"), (4, "GPIO3"), (5, "GPIO0"), (6, "GPIO1"), (7, "GPIO6"),
      (8, "GPIO7"), (9, "GPIO8"), (10, "GPIO9"), (11, "GPIO10"), (12, "GPIO26"), (13, "GPIO25"),
      (14, "5V"), (15, "GND"), (16, "NC")]
j3 = [(1, "GND"), (2, "TX/GPIO11"), (3, "RX/GPIO12"), (4, "GPIO24"), (5, "GPIO23"), (6, "NC/15"),
      (7, "GPIO27"), (8, "GPIO4"), (9, "GPIO5"), (10, "NC"), (11, "GPIO28"), (12, "GND"),
      (13, "GPIO14/D+"), (14, "GPIO13/D-"), (15, "GND"), (16, "NC")]
pins, h = ic(S, 1260, 150, "DK1", "J1 (left)  /  J3 (right)", j1, j3, w=220, pitch=29)
J1NET = {1: "+3V3", 3: "SPARE_IO2", 4: "SPARE_IO3", 5: "G0_SYNC", 6: "G1_DRDY", 7: "G6_SCK",
         8: "G7_LEDC", 9: "G8_MOSI", 10: "G9_MISO", 11: "G10_CS", 14: "DK_5V"}
J3NET = {4: "G24_CS2", 5: "G23_REF", 8: "I2C_SDA", 9: "I2C_SCL"}
for no, net in J1NET.items():
    netlab(S, *pins[f"L{no}"], net, side="l")
x, y = pins["L15"]; S.line(x, y, x - 30, y); gnd(S, x - 30, y)
for no in (2, 12, 13, 16):
    x, y = pins[f"L{no}"]; S.text(x - 4, y + 4, "x", 13, anchor="end")
for no, net in J3NET.items():
    netlab(S, *pins[f"R{no}"], net)
for no in (2, 3, 6, 7, 10, 11, 13, 14, 16):
    x, y = pins[f"R{no}"]; S.text(x + 4, y + 4, "x  (not routed)", 11, c=GRY)
for no in (1, 12, 15):
    x, y = pins[f"R{no}"]; S.line(x, y, x + 20, y); gnd(S, x + 20, y)
S.text(960, 656, "UART0 (J3-2/3) stays free: the WinSDR stream goes out the DevKit's USB-UART port.", 11, c=GRY)
S.text(960, 670, "GPIO25/26 (strapping) unused. Check J1/J3 against your board and the Espressif", 11, c=GRY)
S.text(960, 684, "Dimensions v1.2 DXF before ordering (changed from order PW-2025-04-0446).", 11, c=GRY)

# ── C: ADC 15 Click ───────────────────────────────────────────────────────
panel(S, 20, 520, 900, 560, "C. MikroE ADC 15 Click (ADS131M02) on mikroBUS-pattern sockets J4 / J5")
left = [(1, "AN"), (2, "RST"), (3, "CS"), (4, "SCK"), (5, "MISO"), (6, "MOSI"), (7, "3.3V"), (8, "GND")]
right = [(1, "PWM"), (2, "INT"), (3, "RX"), (4, "TX"), (5, "SCL"), (6, "SDA"), (7, "5V"), (8, "GND")]
cp, ch_ = ic(S, 480, 620, "CLICK", "J4 (mikroBUS 1-8)  /  J5 (16-9)", left, right, w=180, pitch=34)
for no, (ref, esp, adc) in {2: ("R15", "G0_SYNC", "ADC_RST"), 3: ("R13", "G10_CS", "ADC_CS"),
                            4: ("R10", "G6_SCK", "ADC_SCK"), 5: ("R12", "G9_MISO", "ADC_MISO"),
                            6: ("R11", "G8_MOSI", "ADC_MOSI")}.items():
    x, y = cp[f"L{no}"]
    res_h(S, 320, y, ref, SERIES_R[ref], length=110)
    S.line(430, y, x, y)
    netlab(S, 320, y, esp, side="l")
x, y = cp["L1"]; S.text(x - 4, y + 4, "x", 13, anchor="end")
x, y = cp["L7"]; S.line(x, y, 400, y, c=PWR); netlab(S, 400, y, "+3V3A", side="l")
S.dot(420, y); cap_v(S, 420, y, "C7", "100nF", length=40); gnd(S, 420, y + 40)
x, y = cp["L8"]; S.line(x, y, x - 20, y); gnd(S, x - 20, y)
for no, (ref, esp) in {1: ("R16", "G24_CS2"), 2: ("R14", "G1_DRDY")}.items():
    x, y = cp[f"R{no}"]
    res_h(S, x, y, ref, SERIES_R[ref], length=100)
    netlab(S, x + 100, y, esp)
for no in (3, 4, 5, 6):
    x, y = cp[f"R{no}"]; S.text(x + 4, y + 4, "x", 13)
x, y = cp["R7"]; S.text(x + 4, y + 4, "x  not fed: the Click runs on 3.3V", 11, c=GRY)
x, y = cp["R8"]; S.line(x, y, x + 20, y); gnd(S, x + 20, y)
S.text(40, 926, "Series resistors sit at the driving end: R10, R11, R13, R15, R16 next to the DevKit, R12 (MISO)", 11, c=GRY)
S.text(40, 940, "and R14 (DRDY) next to the Click. 330R on the slow lines CS, RST and CS2 (R13, R15, R16): they idle", 11, c=GRY)
S.text(40, 954, "high, so with the DevKit on USB and the carrier off they would back-power the Click through its", 11, c=GRY)
S.text(40, 968, "ESD diodes; 330R keeps that under 10 mA per pin. Low level against the Click's 4.7k pull-ups: 0.22 V.", 11, c=GRY)
S.text(40, 982, "The Click's 4.7k pull-ups on RST, CS, PWM (CS2) and INT go to its VCC = +3V3A. PWM drives the LTC6903 SEN.", 11, c=GRY)
S.text(40, 1000, "The analog inputs are NOT on mikroBUS: they are the Click's own TB1 / TB2 screw terminals,", 11, c=PWR)
S.text(40, 1014, "wired from P4 and P5 (section D). CLKIN comes from P6 (section E) after the trace cut.", 11, c=PWR)
S.text(40, 1036, "mikroBUS sockets: 2 x 1x8, 22.86 mm row spacing. Check against the MikroE mikroBUS spec", 11, c=GRY)
S.text(40, 1050, "and the ADC 15 Click drawing before ordering.", 11, c=GRY)

# ── D: analog front end ───────────────────────────────────────────────────
panel(S, 20, 1100, 900, 530, "D. Analog front end: one block per EQAMP channel, +-10 V to +-0.99 V, to the Click")
S.text(40, 1142, "Each EQAMP channel output is a 2-pin pair (signal, ground). Each one keeps its pair all the way to", 11, c=GRY)
S.text(40, 1156, "its ADC input: signal through the divider to AINx+, the channel's own ground return to AINx-.", 11, c=GRY)
CH = (("P2", "CH1 vertical (Z)", "R20", "R21", "C10", "AIN0P", "P4", "TO CLICK TB1", ["AIN0+", "AIN0-"], 0,
       "AMP_GND_Z", "R30"),
      ("P3", "CH2 horizontal (H)", "R22", "R23", "C11", "AIN1P", "P5", "TO CLICK TB2", ["AIN1-", "AIN1+"], 1,
       "AMP_GND_H", "R34"))
for i, (pin_ref, title, rs, rp, cc, node, out_ref, out_title, out_pins, sig_at, agnd, rlink) in enumerate(CH):
    y0 = 1200 + i * 185
    pi = terminal(S, 100, y0, pin_ref, title, ["SIG", "GND"], pitch=40)
    po = terminal(S, 760, y0, out_ref, out_title, out_pins, side="l", pitch=40)
    ysig, ygnd = pi[0][1], pi[1][1]
    rail = ygnd + 60
    res_h(S, pi[0][0], ysig, rs, "9.1k 0.1%", length=130)
    S.line(pi[0][0] + 130, ysig, 600, ysig)
    S.dot(320, ysig); res_v(S, 320, ysig, rp, "1.0k 0.1%", length=rail - ysig); S.dot(320, rail)
    S.dot(440, ysig); cap_v(S, 440, ysig, cc, "47nF C0G", length=rail - ysig); S.dot(440, rail)
    S.text(530, ysig - 8, node, 10, c=NET, anchor="middle")
    xs, ys = po[sig_at]
    S.line(600, ysig, 600, ys); S.line(600, ys, xs, ys)
    xg, yg = po[1 - sig_at]
    S.line(xg, yg, 690, yg); netlab(S, 690, yg, agnd, side="l")
    S.line(pi[1][0], ygnd, 160, ygnd); S.line(160, ygnd, 160, rail); S.line(160, rail, 500, rail)
    netlab(S, 500, rail, agnd)
    S.dot(240, rail); res_v(S, 240, rail, rlink, "10R", length=34, dnp=True); gnd(S, 240, rail + 34)
S.text(40, 1580, "AMP_GND_Z and AMP_GND_H are separate nets: each channel's ground return runs only from its input", 11, c=GRY)
S.text(40, 1594, "terminal to its AINx-. They meet at the EQAMP. R30, R34 DNP: the J8 power return already joins amplifier", 11, c=GRY)
S.text(40, 1608, "and carrier grounds; fit 10R only if hum shows up. Ratio 0.099: +-10 V -> +-0.99 V. No clamp diodes:", 11, c=GRY)
S.text(40, 1622, "a clamp to GND would clip the negative half. TB2 is AIN1-, AIN1+ on the Click (MikroE schematic v100).", 11, c=GRY)

# ── E: ADC clock ──────────────────────────────────────────────────────────
panel(S, 940, 720, 1000, 360, "E. ADC clock: 8.192 MHz oscillator or ESP32 LEDC, to the Click CLKIN pad")
xpins, _ = ic(S, 1120, 800, "Y1", "CB3LV-3I-8M192000, 7 x 5 mm", [(4, "VCC"), (1, "EN")], [(3, "OUT"), (2, "GND")],
              w=150, pitch=60)
x, y = xpins["L4"]; S.line(x, y, x - 40, y, c=PWR); netlab(S, x - 40, y, "+3V3A", side="l")
S.dot(x - 20, y); cap_v(S, x - 20, y, "C12", "100nF", length=26); gnd(S, x - 20, y + 26)
x, y = xpins["L1"]; res_h(S, x - 70, y, "R31", "10k", length=70); netlab(S, x - 70, y, "+3V3A", side="l")
x, y = xpins["R2"]; S.line(x, y, x + 20, y); gnd(S, x + 20, y)
xo = xpins["R3"]
S.line(*xo, 1330, xo[1]); S.text(1290, xo[1] - 6, "XO_OUT", 10, c=NET)
S.rect(1330, 800, 30, 90, fill="#f4f4f4"); S.text(1345, 790, "JP1", 12, anchor="middle", bold=True)
for k, lab in enumerate(("1 XO", "2 SEL", "3 LEDC")):
    S.circle(1345, 815 + k * 30, 6); S.text(1368, 819 + k * 30, lab, 11)
S.line(1330, 845, 1310, 845); S.line(1310, 845, 1310, 960)
res_h(S, 1310, 960, "R33", "33R", length=100)
S.text(1450, 952, "CLKIN", 10, c=NET)
S.rect(1520, 940, 34, 54, fill="#f4f4f4"); S.text(1537, 930, "P6", 12, anchor="middle", bold=True)
S.circle(1537, 960, 6); S.circle(1537, 980, 6)
S.line(1410, 960, 1520, 960); S.text(1562, 964, "1 CLKIN -> Click CLKIN pad", 11)
S.text(1562, 984, "2 GND (twisted pair)", 11); S.line(1520, 980, 1500, 980); gnd(S, 1500, 980)
S.line(1345, 881, 1345, 1030); S.line(1345, 1030, 1420, 1030)
res_h(S, 1420, 1030, "R32", "33R", length=90); netlab(S, 1510, 1030, "G7_LEDC")
S.text(960, 1062, "JP1 1-2: oscillator (exactly 1000 SPS at OSR 4096). 2-3: GPIO7 LEDC, 8.000 MHz. Open: stock LTC6903,", 11, c=GRY)
S.text(960, 1076, "P6 unwired and the Click trace left intact. Y1 EN: open or high = running, R31 holds it high.", 11, c=GRY)

# ── F: timing ─────────────────────────────────────────────────────────────
panel(S, 940, 1100, 1000, 470, "F. Timing: DS3231 (ZS-042) and GPS PPS, one of them to GPIO23")
p = terminal(S, 1060, 1190, "J2", "DS3231 ZS-042", ["32K", "SQW", "SCL", "SDA", "VCC", "GND"], pitch=30)
x, y = p[0]; S.text(x + 6, y + 4, "x", 13)
for i, net in enumerate(("RTC_SQW", "I2C_SCL", "I2C_SDA", "+3V3")):
    x, y = p[i + 1]
    S.line(x, y, x + 90, y)
    netlab(S, x + 90, y, net)
x, y = p[5]; S.line(x, y, x + 30, y); gnd(S, x + 30, y)
S.text(960, 1400, "ZS-042 brings its own 4.7k pull-ups on SCL, SDA and SQW,", 11, c=GRY)
S.text(960, 1414, "so R40/R41 stay DNP. 32K unused. Its charge circuit", 11, c=GRY)
S.text(960, 1428, "needs an LIR2032, or remove its diode for a CR2032.", 11, c=GRY)
S.text(960, 1450, "C13 100nF at J2 VCC. R40, R41 4.7k DNP (SDA, SCL to +3V3).", 11, c=GRY)
p5 = terminal(S, 1500, 1190, "P7", "GPS", ["+5V", "GND", "PPS"], side="l", pitch=34)
x, y = p5[2]
res_h(S, x - 120, y, "R42", "1k", length=120)
S.text(x - 60, y + 26, "PPS_IN", 10, c=NET, anchor="middle")
S.dot(x - 120, y); res_v(S, x - 120, y, "R43", "2.2k", length=60, dnp=True); gnd(S, x - 120, y + 60)
S.line(x - 120, y, x - 150, y); netlab(S, x - 150, y, "PPS_IN", side="l")
x, y = p5[0]; S.line(x, y, x - 30, y); netlab(S, x - 30, y, "+5V", side="l")
x, y = p5[1]; S.line(x, y, x - 20, y); gnd(S, x - 20, y)
S.rect(1640, 1300, 90, 30, fill="#f4f4f4"); S.text(1685, 1292, "JP2", 12, anchor="middle", bold=True)
for k, lab in enumerate(("SQW", "REF", "PPS")):
    S.circle(1655 + k * 30, 1315, 6); S.text(1655 + k * 30, 1346, lab, 10, anchor="middle")
S.line(1685, 1350, 1685, 1400); netlab(S, 1685, 1400, "G23_REF")
S.line(1655, 1321, 1655, 1440); netlab(S, 1655, 1440, "RTC_SQW", side="l")
S.line(1715, 1321, 1715, 1440); netlab(S, 1715, 1440, "PPS_IN")
S.dot(1685, 1380); S.line(1685, 1380, 1780, 1380); res_v(S, 1780, 1320, "R44", "10k", length=60)
pwr(S, 1780, 1320, "+3V3")
S.text(960, 1520, "JP2 1-2: DS3231 SQW, 2-3: GPS PPS. R43 makes a 1k/2.2k divider for a 5 V PPS.", 11, c=GRY)
S.text(960, 1534, "P7 +5V powers the GPS module (u-blox class boards take 3.3-5 V).", 11, c=GRY)

# ── G: spare, test points ─────────────────────────────────────────────────
panel(S, 20, 1650, 1920, 200, "G. Spare GPIO, test points, mounting")
terminal(S, 80, 1720, "P8", "SPARE", ["IO2", "IO3", "+3V3", "GND"], pitch=26)
S.text(200, 1740, "IO2 and IO3 are strapping pins on some DevKit revisions: inputs or late outputs only.", 11, c=GRY)
S.text(200, 1760, "Test points (1x40 header pieces): TP1 +VIN, TP2 +5V, TP3 +3V3, TP4 +3V3A, TP5 GND, TP6 CLKIN.", 11, c=GRY)
S.text(200, 1780, "Four M3 holes, nylon standoffs. Board 130 x 90 mm, 2 layers.", 11, c=GRY)
S.save("seismo-carrier-schematic.svg")

# ══ PCB PLACEMENT ═════════════════════════════════════════════════════════
K = 8.0
BW, BH = 130, 90
OX, OY = 50, 110
P = Svg(int(OX + BW * K + 420), int(OY + BH * K + 80),
        "VSEISMO carrier board, PCB placement rev A (top view, grid 5 mm, not for fabrication)")
P.text(20, 60, "Placement and zoning only: no routing. Import seismo-carrier.net into KiCad, then route and run DRC.",
       13, c=GRY)


def mm(x, y):
    return OX + x * K, OY + y * K


for gx in range(0, BW + 1, 5):
    x, _ = mm(gx, 0); P.line(x, OY, x, OY + BH * K, c="#eee", w=1)
for gy in range(0, BH + 1, 5):
    _, y = mm(0, gy); P.line(OX, y, OX + BW * K, y, c="#eee", w=1)
P.rect(OX, OY, BW * K, BH * K, c="#1b5e20", sw=3, rx=10)
P.text(OX, OY - 10, f"Board {BW} x {BH} mm, 2 layers, 1.6 mm FR-4, 1 oz copper", 13, c="#1b5e20")
for hx, hy in ((4, 4), (BW - 4, 4), (4, BH - 4), (BW - 4, BH - 4)):
    x, y = mm(hx, hy)
    P.circle(x, y, 1.6 * K, fill="#ddd")
    P.circle(x, y, 3.2 * K, c=GRY, sw=1)


def box(x, y, w, h, ref, sub="", c=INK, fill="#fff", size=11):
    X, Y = mm(x, y)
    P.rect(X, Y, w * K, h * K, c=c, fill=fill, sw=1.6)
    P.text(X + w * K / 2, Y + h * K / 2 + (-2 if sub else 4), ref, size, anchor="middle", bold=True, c=c)
    if sub:
        P.text(X + w * K / 2, Y + h * K / 2 + 12, sub, 9, anchor="middle", c=GRY)


TG = "#2e7d32"


def term(x, y, n, ref, label, vertical=True, lab_side="r", pitch=5.08):
    w, h = (8, n * pitch) if vertical else (n * pitch, 8)
    X, Y = mm(x, y)
    P.rect(X, Y, w * K, h * K, c=TG, fill="#e8f5e9", sw=1.6)
    for i in range(n):
        cx, cy = (mm(x + 4, y + pitch / 2 + i * pitch) if vertical
                  else mm(x + pitch / 2 + i * pitch, y + 4))
        P.circle(cx, cy, 9 if pitch > 3 else 6, c=TG, fill="#fff")
        P.text(cx, cy + 4, str(i + 1), 9, anchor="middle", c=TG)
    if vertical and lab_side == "b":
        P.text(X + w * K / 2, Y + h * K + 14, ref, 12, anchor="middle", bold=True, c=TG)
        P.text(X + w * K / 2, Y + h * K + 27, label, 10, anchor="middle", c=TG)
    elif vertical:
        lx = X + w * K + 6 if lab_side == "r" else X - 6
        anc = "start" if lab_side == "r" else "end"
        P.text(lx, Y + 12, ref, 12, anchor=anc, bold=True, c=TG)
        P.text(lx, Y + 26, label, 10, anchor=anc, c=TG)
    else:
        P.text(X + w * K / 2, Y - 18, ref, 12, anchor="middle", bold=True, c=TG)
        P.text(X + w * K / 2, Y - 5, label, 10, anchor="middle", c=TG)


def zone(x, y, w, h, c, label):
    X, Y = mm(x, y)
    P.rect(X, Y, w * K, h * K, c=c, dash="7 5", sw=1.4)
    P.text(X + 6, Y + 14, label, 10, c=c)


def module(x, y, w, h, ref, sub, c):
    X, Y = mm(x, y)
    P.rect(X, Y, w * K, h * K, c=c, fill="#f3e5f5", sw=2, dash="10 4")
    P.text(X + w * K / 2, Y + h * K / 2 - 6, ref, 15, anchor="middle", bold=True, c=c)
    P.text(X + w * K / 2, Y + h * K / 2 + 10, sub, 10, anchor="middle", c=c)


def sockets(x, y, n, c, name):
    for i in range(n):
        px, py = mm(x, y + i * 2.54)
        P.rect(px - 4, py - 4, 8, 8, c=c, fill="#fff", sw=1)
    px, py = mm(x, y - 1.6)
    P.text(px, py, name, 10, anchor="middle", bold=True, c=c)


VI = "#6a1b9a"
# ── zones
zone(1.5, 1.5, 32.5, 40, NET, "analog: P2-P5, dividers, RC")
zone(1.5, 62, 60, 26.5, PWR, "power: star GND at P1, U1 far from analog")
# ── analog front end, left edge
term(2, 9, 2, "P2", "CH1 Z")
term(2, 23, 2, "P3", "CH2 H")
box(17, 4, 7, 14, "R20-23", "0.1 %", c=NET, size=10)
box(17, 20, 7, 6, "C10 C11", "C0G", c=NET, size=9)
box(17, 29, 9, 3, "R30 R34", "", c=NET, size=9)
term(25.5, 3, 2, "P4", "to TB1", lab_side="b")
term(25.5, 20, 2, "P5", "to TB2", lab_side="b")
# ── Click
module(35, 2, 25.4, 42.9, "ADC 15 Click", "MIKROE-4890", VI)
X, Y = mm(37, 3); P.rect(X, Y, 21 * K, 7 * K, c=VI, fill="#fff", sw=1)
P.text(X + 10.5 * K, Y + 4.5 * K, "Click TB1 / TB2 (AIN)", 9, anchor="middle", c=VI)
sockets(36.27, 25, 8, VI, "J4"); sockets(59.13, 25, 8, VI, "J5")
P.text(*mm(47.7, 14.5), "mikroBUS rows 22.86 mm apart", 9, anchor="middle", c=GRY)
# ── clock under the Click
box(37, 47, 9, 6, "Y1", "CB3LV 7x5", c=VI, size=10)
box(49, 47, 3, 7.6, "", "", c=VI); P.text(*mm(48.6, 57), "JP1", 9, c=VI)
box(54, 47, 7, 3, "R31-33", "", size=8)
term(54, 53, 2, "P6", "CLKIN", vertical=False, pitch=2.54)
box(40, 55, 4, 3, "C12", "", size=8)
# ── analog 3.3 V next to the Click 3.3V pin
box(14, 44, 6, 3, "FB1", "", size=9)
box(14, 48, 6, 6, "C4", "", size=9)
box(21, 44, 7, 5, "U2", "TO-92", size=9)
box(21, 50, 7, 4, "C5 C6 C7", "", size=8)
# ── series resistors between Click and DevKit
box(62.5, 16, 7, 28, "R10-R16", "33R", size=9)
# ── DevKit
dkx, dky, dkw, dkh = 72, 1, 26, 60
module(dkx, dky, dkw, dkh, "DK1", "ESP32-C5-DevKitC-1", VI)
X, Y = mm(dkx, dky)
for i, t in enumerate(("USB-UART", "USB")):
    ux = X + (3 + i * 12) * K
    P.rect(ux, Y - 2.5 * K, 9 * K, 5 * K, c=VI, fill="#fff")
    P.text(ux + 4.5 * K, Y - 3.2 * K, t, 9, anchor="middle", c=VI)
sockets(dkx + 1.5, dky + 6, 16, VI, "J1"); sockets(dkx + dkw - 1.5, dky + 6, 16, VI, "J3")
X, Y = mm(67, 47); P.rect(X, Y, 36 * K, 16 * K, c="#c62828", fill="url(#hatch)", sw=1.4)
P.el.insert(1, "<defs><pattern id='hatch' width='8' height='8' patternUnits='userSpaceOnUse' "
               "patternTransform='rotate(45)'><line x1='0' y1='0' x2='0' y2='8' stroke='#e57373' "
               "stroke-width='2'/></pattern></defs>")
P.rect(X + 3 * K, Y + 5.5 * K, 30 * K, 5 * K, c="#c62828", fill="#fff", sw=1)
P.text(X + 18 * K, Y + 8 * K + 2, "ANTENNA KEEP-OUT", 11, anchor="middle", bold=True, c="#c62828")
P.text(X + 18 * K, Y + 8 * K + 14, "no copper, both layers", 9, anchor="middle", c="#c62828")
# ── timing, right side
X, Y = mm(105, 6); P.rect(X, Y, 2.54 * K, 6 * 2.54 * K, c=INK, fill="#fff", sw=1.6)
for i in range(6):
    P.circle(X + 1.27 * K, Y + (1.27 + i * 2.54) * K, 5)
P.text(X + 1.27 * K, Y - 6, "J2 DS3231", 10, anchor="middle", bold=True)
box(109, 6, 7, 6, "R40 R41", "DNP", size=8)
box(109, 14, 4, 3, "C13", "", size=8)
box(105, 26, 7.6, 3, "", ""); P.text(*mm(105, 31.5), "JP2 SQW/PPS", 9)
box(113, 24, 6, 7, "R42-44", "", size=8)
term(120, 20, 3, "P7", "GPS", lab_side="l")
term(120, 40, 4, "P8", "SPARE", lab_side="l")
# ── power zone
term(2, 66, 2, "P1", "DC IN J8")
box(16, 64, 8, 3.5, "F1", "", size=9)
box(16, 69, 9, 3, "D3", "", size=9)
box(16, 74, 9, 3, "D1", "TVS", size=9)
box(28, 64, 6, 6, "C1", "22uF", size=9)
box(28, 76, 12, 7, "U1", "OKI-78SR", size=10)
box(42, 64, 7, 7, "C2", "100uF", size=9)
box(42, 73, 4, 3, "C3", "", size=8)
box(51, 64, 5, 3, "", ""); P.text(*mm(51, 70), "LED1 R1", 9)
box(51, 74, 10, 3, "D2", "1N5817", size=8)
P.text(*mm(40, 85), "D2 cathode to J1-14 (5V) by a 1 mm", 9, c=GRY)
P.text(*mm(40, 87), "trace around the keep-out", 9, c=GRY)
for i in range(6):
    X, Y = mm(104 + i * 3.2, 72)
    P.circle(X, Y, 6, fill="#ffeb3b")
P.text(*mm(102, 76), "TP1-TP6: VIN 5V 3V3 3V3A GND CLKIN", 9, c=GRY)
# ── legend
lx = OX + BW * K + 30
ly = OY + 6
rules = [
    ("Layout rules", True),
    ("Bottom: solid GND pour, stitched to", False),
    ("  top GND fill, vias every ~10 mm.", False),
    ("AMP_GND_Z: P2-2 to P4-2 only.", False),
    ("AMP_GND_H: P3-2 to P5-1 only.", False),
    ("  Own traces, not the pour. R30/R34 DNP.", False),
    ("No GND pour under R20-R23, C10-C11.", False),
    ("U1 loop small, far from the analog zone,", False),
    ("  the Click and the antenna.", False),
    ("+3V3A: U2 to J4-7 and Y1 only, 0.5 mm.", False),
    ("VIN, 5V: 1 mm. Signals 0.3 mm.", False),
    ("SPI and DRDY: short, over solid GND,", False),
    ("  series R at the driving end.", False),
    ("CLKIN: Y1 close to JP1 and P6.", False),
    ("Nothing under the DevKit antenna.", False),
    ("Terminal wire entry faces the edge.", False),
    ("", False),
    ("Colours", True),
    ("green   terminals and pin headers", False),
    ("purple  plug-in modules and sockets", False),
    ("blue    analog zone", False),
    ("red     power zone", False),
    ("hatch   antenna keep-out", False),
    ("yellow  test points", False),
    ("", False),
    ("Before ordering", True),
    ("DevKit outline and header rows:", False),
    ("  Espressif DXF v1.2.", False),
    ("Click outline and mikroBUS rows:", False),
    ("  MikroE drawing.", False),
    ("Print 1:1 and test-fit DevKit,", False),
    ("  Click, ZS-042 and the terminals.", False),
]
for t, b in rules:
    P.text(lx, ly, t, 13 if b else 11, bold=b, c=INK if b else "#333")
    ly += 20 if b else 16
P.save("seismo-carrier-pcb.svg")

# ══ PARTS AND NETLIST ═════════════════════════════════════════════════════
# One entry per component: ref, value, KiCad footprint, symbol (lib, part),
# and {pad number: net}. Pads left out are unconnected. Footprints are from
# the KiCad 8/9 standard libraries. Pad conventions: LED and diode pad 1 =
# cathode, CP pad 1 = +.
FP_R = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal"
FP_C = "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm"
FP_CP = "Capacitor_THT:CP_Radial_D6.3mm_P2.50mm"
FP_CP5 = "Capacitor_THT:CP_Radial_D5.0mm_P2.00mm"
FP_D = "Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal"
FP_LED = "LED_THT:LED_D3.0mm"
FP_PTC = "Capacitor_THT:C_Disc_D7.5mm_W5.0mm_P5.00mm"
FP_HDR = "Connector_PinHeader_2.54mm:PinHeader_1x{n:02d}_P2.54mm_Vertical"
FP_SKT = "Connector_PinSocket_2.54mm:PinSocket_1x{n:02d}_P2.54mm_Vertical"
FP_TB = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-{n}-5.08_1x{n:02d}_P5.08mm_Horizontal"

PARTS = []


def part(ref, value, fp, sym, pads, note=""):
    PARTS.append((ref, value, fp, sym, {str(k): v for k, v in pads.items()}, note))


def res(ref, value, a, b, note=""):
    part(ref, value, FP_R, ("Device", "R"), {1: a, 2: b}, note)


def cap(ref, value, a, b, note=""):
    part(ref, value, FP_C, ("Device", "C"), {1: a, 2: b}, note)


def tb(ref, value, nets):
    part(ref, value, FP_TB.format(n=len(nets)), ("Connector", f"Screw_Terminal_01x{len(nets):02d}"),
         {i + 1: n for i, n in enumerate(nets) if n})


# A: power
tb("P1", "DC IN EQAMP J8", ["+VIN_RAW", "GND"])
part("F1", "RXEF050", FP_PTC, ("Device", "Polyfuse"), {1: "+VIN_RAW", 2: "+VIN_F"},
     "footprint is a stand-in, check the RXEF050 drawing")
part("D3", "1N5819", FP_D, ("Device", "D_Schottky"), {1: "+VIN", 2: "+VIN_F"}, "reverse-polarity block")
part("D1", "P6KE33A", "Diode_THT:D_DO-15_P10.16mm_Horizontal", ("Device", "D_TVS"),
     {1: "+VIN", 2: "GND"}, "TVS, unidirectional, 28.2 V standoff (J8 reaches about 26.6 V), cathode to +VIN")
part("C1", "22uF 50V", FP_CP, ("Device", "C_Polarized"), {1: "+VIN", 2: "GND"})
part("U1", "OKI-78SR-5/1.5-W36-C", "Converter_DCDC:Converter_DCDC_Murata_OKI-78SR_Vertical",
     ("Regulator_Switching", "OKI-78SR-5_1.5-W36-C"), {1: "+VIN", 2: "GND", 3: "+5V"},
     "pins IN-GND-OUT; check caps against the Murata datasheet")
part("C2", "100uF 25V", FP_CP, ("Device", "C_Polarized"), {1: "+5V", 2: "GND"})
cap("C3", "100nF", "+5V", "GND")
res("R1", "2.2k", "+5V", "LED1_A")
part("LED1", "green", FP_LED, ("Device", "LED"), {1: "GND", 2: "LED1_A"})
part("D2", "1N5817", FP_D, ("Device", "D_Schottky"), {1: "DK_5V", 2: "+5V"})
part("FB1", "ferrite bead", FP_R, ("Device", "FerriteBead"), {1: "+5V", 2: "+5V_A"},
     "axial bead; resistor footprint as a stand-in, check the bead drawing")
part("C4", "10uF 16V", FP_CP5, ("Device", "C_Polarized"), {1: "+5V_A", 2: "GND"})
part("U2", "LP2950ACZ-3.3", "Package_TO_SOT_THT:TO-92_Inline", ("Regulator_Linear", "LP2950-3.3_TO92"),
     {1: "+3V3A", 2: "GND", 3: "+5V_A"}, "TO-92 1 OUT, 2 GND, 3 IN; verify against the datasheet")
part("C5", "10uF 16V", FP_CP5, ("Device", "C_Polarized"), {1: "+3V3A", 2: "GND"}, "LP2950 needs >= 1 uF")
cap("C6", "100nF", "+3V3A", "GND")

# B: DevKit, two 1x16 sockets (v1.2 pinout)
part("J1", "DevKit J1", FP_SKT.format(n=16), ("Connector", "Conn_01x16_Socket"),
     {**J1NET, 15: "GND"})
part("J3", "DevKit J3", FP_SKT.format(n=16), ("Connector", "Conn_01x16_Socket"),
     {**J3NET, 1: "GND", 12: "GND", 15: "GND"})

# C: ADC 15 Click on mikroBUS-pattern sockets
part("J4", "Click mikroBUS 1-8", FP_SKT.format(n=8), ("Connector", "Conn_01x08_Socket"),
     {2: "ADC_RST", 3: "ADC_CS", 4: "ADC_SCK", 5: "ADC_MISO", 6: "ADC_MOSI", 7: "+3V3A", 8: "GND"},
     "1 AN not connected")
part("J5", "Click mikroBUS 16-9", FP_SKT.format(n=8), ("Connector", "Conn_01x08_Socket"),
     {1: "ADC_CS2", 2: "ADC_DRDY", 8: "GND"},
     "pad 1 = PWM (16) .. pad 8 = GND (9); RX TX SCL SDA 5V not connected")
res("R10", SERIES_R["R10"], "G6_SCK", "ADC_SCK")
res("R11", SERIES_R["R11"], "G8_MOSI", "ADC_MOSI")
res("R12", SERIES_R["R12"], "ADC_MISO", "G9_MISO", "place at the Click")
res("R13", SERIES_R["R13"], "G10_CS", "ADC_CS", "330R: idles high, limits back-power into an unpowered Click")
res("R14", SERIES_R["R14"], "ADC_DRDY", "G1_DRDY", "place at the Click")
res("R15", SERIES_R["R15"], "G0_SYNC", "ADC_RST", "330R: idles high, limits back-power into an unpowered Click")
res("R16", SERIES_R["R16"], "G24_CS2", "ADC_CS2", "330R: idles high, limits back-power into an unpowered Click")
cap("C7", "100nF", "+3V3A", "GND", "at J4 pin 7")

# D: analog front end
# One ground net per channel: each return runs only from its input terminal
# to its AINx-, and the two meet at the EQAMP, not on the carrier.
tb("P2", "CH1 Z IN", ["AMP_Z", "AMP_GND_Z"])
tb("P3", "CH2 H IN", ["AMP_H", "AMP_GND_H"])
res("R20", "9.1k 0.1%", "AMP_Z", "AIN0P", "0.1 %, 25 ppm metal film")
res("R21", "1.0k 0.1%", "AIN0P", "AMP_GND_Z", "0.1 %, 25 ppm metal film")
cap("C10", "47nF C0G", "AIN0P", "AMP_GND_Z", "C0G/NP0 only")
res("R22", "9.1k 0.1%", "AMP_H", "AIN1P", "0.1 %, 25 ppm metal film")
res("R23", "1.0k 0.1%", "AIN1P", "AMP_GND_H", "0.1 %, 25 ppm metal film")
cap("C11", "47nF C0G", "AIN1P", "AMP_GND_H", "C0G/NP0 only")
tb("P4", "TO CLICK TB1", ["AIN0P", "AMP_GND_Z"])        # Click TB1: 1 AIN0+, 2 AIN0-
tb("P5", "TO CLICK TB2", ["AMP_GND_H", "AIN1P"])        # Click TB2: 1 AIN1-, 2 AIN1+
res("R30", "10R", "AMP_GND_Z", "GND", "DNP by default: the J8 power return joins the grounds; fit only if hum")
res("R34", "10R", "AMP_GND_H", "GND", "DNP by default: the J8 power return joins the grounds; fit only if hum")

# E: ADC clock
part("Y1", "CB3LV-3I-8M192000", "seismo:Oscillator_CTS_CB3LV_7.0x5.0mm", ("Oscillator", "ASE-xxxMHz"),
     {1: "XO_EN", 2: "GND", 3: "XO_OUT", 4: "+3V3A"},
     "CTS 8.192 MHz 3.3 V HCMOS, +-50 ppm, -40..85 C, 1 EN 2 GND 3 OUT 4 VCC; footprint in seismo.pretty, "
     "CTS 008-0256-0 Rev H land pattern")
cap("C12", "100nF", "+3V3A", "GND", "at Y1 pin 4, datasheet asks >= 10 nF")
res("R31", "10k", "XO_EN", "+3V3A")
res("R32", "33R", "G7_LEDC", "LEDC_CLK")
part("JP1", "CLK SEL", FP_HDR.format(n=3), ("Jumper", "Jumper_3_Open"),
     {1: "XO_OUT", 2: "CLK_SEL", 3: "LEDC_CLK"}, "1-2 = oscillator, 2-3 = GPIO7 LEDC, open = stock LTC6903")
res("R33", "33R", "CLK_SEL", "CLKIN")
part("P6", "CLKIN", FP_HDR.format(n=2), ("Connector", "Conn_01x02_Pin"), {1: "CLKIN", 2: "GND"},
     "twisted pair to the Click CLKIN pad")

# F: timing
part("J2", "DS3231 ZS-042", FP_SKT.format(n=6), ("Connector", "Conn_01x06_Socket"),
     {2: "RTC_SQW", 3: "I2C_SCL", 4: "I2C_SDA", 5: "+3V3", 6: "GND"},
     "module order 32K SQW SCL SDA VCC GND; 32K not connected")
cap("C13", "100nF", "+3V3", "GND", "at J2 VCC")
res("R40", "4.7k", "I2C_SDA", "+3V3", "DNP: ZS-042 has its own")
res("R41", "4.7k", "I2C_SCL", "+3V3", "DNP: ZS-042 has its own")
tb("P7", "GPS", ["+5V", "GND", "GPS_PPS"])
res("R42", "1k", "GPS_PPS", "PPS_IN")
res("R43", "2.2k", "PPS_IN", "GND", "DNP; fit for a 5 V PPS output")
part("JP2", "REF SEL", FP_HDR.format(n=3), ("Jumper", "Jumper_3_Open"),
     {1: "RTC_SQW", 2: "G23_REF", 3: "PPS_IN"}, "1-2 = DS3231 SQW, 2-3 = GPS PPS")
res("R44", "10k", "G23_REF", "+3V3")

# G: spare, test points
tb("P8", "SPARE", ["SPARE_IO2", "SPARE_IO3", "+3V3", "GND"])
for i, n in enumerate(("+VIN", "+5V", "+3V3", "+3V3A", "GND", "CLKIN")):
    part(f"TP{i+1}", n, FP_HDR.format(n=1), ("Connector", "TestPoint"), {1: n})

# ── checks
refs = [p[0] for p in PARTS]
dups = {r for r in refs if refs.count(r) > 1}
assert not dups, f"duplicate refs {dups}"
nets = {}
for ref, _v, _f, _s, pads, _n in PARTS:
    for pad, net in pads.items():
        nets.setdefault(net, []).append((ref, pad))
lonely = [n for n, m in nets.items() if len(m) < 2]
assert not lonely, f"nets with one member: {lonely}"


def natkey(s):
    import re
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", s)]


def q(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


NS = uuid.UUID("5e15e0c1-c5a0-4d31-8a2b-0aadc15c0001")
order = sorted(nets, key=lambda n: (n != "GND", not n.startswith("+"), natkey(n)))
out = ["(export (version \"E\")",
       "  (design",
       f"    (source \"gen_seismo_carrier.py\")",
       f"    (date {q(datetime.date.today().isoformat())})",
       "    (tool \"gen_seismo_carrier.py\")",
       "    (sheet (number \"1\") (name \"/\") (tstamps \"/\")",
       "      (title_block (title \"VSEISMO carrier board\") (company \"\") (rev \"A\")"
       " (date \"\") (source \"gen_seismo_carrier.py\"))))",
       "  (components"]
for ref, val, fp, (lib, sym), _pads, _note in sorted(PARTS, key=lambda p: natkey(p[0])):
    out.append(f"    (comp (ref {q(ref)})")
    out.append(f"      (value {q(val)})")
    out.append(f"      (footprint {q(fp)})")
    out.append(f"      (libsource (lib {q(lib)}) (part {q(sym)}) (description \"\"))")
    out.append("      (sheetpath (names \"/\") (tstamps \"/\"))")
    out.append(f"      (tstamps {q(str(uuid.uuid5(NS, ref)))}))")
out.append("  )")
out.append("  (nets")
for code, net in enumerate(order, 1):
    out.append(f"    (net (code {q(str(code))}) (name {q('/' + net if not net.startswith('+') and net != 'GND' else net)})")
    for ref, pad in sorted(nets[net], key=lambda m: (natkey(m[0]), natkey(m[1]))):
        out.append(f"      (node (ref {q(ref)}) (pin {q(pad)}))")
    out[-1] += ")"
out.append("  )")
out.append(")")
with open(os.path.join(OUT, "seismo-carrier.net"), "w") as f:
    f.write("\n".join(out) + "\n")


def front(title, what):
    return ["---", f"title: {title}", f"date: {datetime.date.today().isoformat()}",
            "tags: [esp32, seismograph, carrier, kicad, generated-table]", "status: active", "---", "",
            f"# {title}", "", f"{what} Written by `gen_seismo_carrier.py`; edit the script, not this note.",
            "Part of [[Seismo Carrier Board]].", ""]


md = front("Seismo Carrier Netlist", "Carrier rev A nets, from the same parts list as `seismo-carrier.net`.")
md += ["| Net | Members |", "|---|---|"]
for net in order:
    md.append(f"| {net} | " + ", ".join(f"{r}-{p}" for r, p in
                                       sorted(nets[net], key=lambda m: (natkey(m[0]), natkey(m[1])))) + " |")
with open(os.path.join(OUT, "Seismo Carrier Netlist.md"), "w") as f:
    f.write("\n".join(md) + "\n")

md = front("Seismo Carrier Footprints", "KiCad footprint per part for the carrier rev A netlist.")
md += ["| Ref | Value | Footprint | Note |", "|---|---|---|---|"]
for ref, val, fp, _s, _p, note in sorted(PARTS, key=lambda p: natkey(p[0])):
    md.append(f"| {ref} | {val} | `{fp}` | {note} |")
with open(os.path.join(OUT, "Seismo Carrier Footprints.md"), "w") as f:
    f.write("\n".join(md) + "\n")

# ── project footprint library: seismo.pretty + fp-lib-table ───────────────
# Y1, CTS CB3/CB3LV 7.0 x 5.0 mm. Land pattern from CTS 008-0256-0 Rev H,
# "Recommended Pad Layout": four 1.8 x 2.0 mm pads, 5.08 mm apart in X and
# 4.20 mm in Y; top view, pin 1 bottom left, numbered anticlockwise (2 bottom
# right, 3 top right, 4 top left, next to the bypass capacitor).
def kicad_fp_cb3lv():
    name = "Oscillator_CTS_CB3LV_7.0x5.0mm"
    px, py, pw, ph = 2.54, 2.10, 1.8, 2.0
    pads = {1: (-px, py), 2: (px, py), 3: (px, -py), 4: (-px, -py)}

    def line(x1, y1, x2, y2, layer, w):
        return (f"  (fp_line (start {x1:g} {y1:g}) (end {x2:g} {y2:g}) "
                f"(stroke (width {w:g}) (type solid)) (layer \"{layer}\"))")

    def text(kind, txt, x, y, layer):
        return (f"  (fp_text {kind} \"{txt}\" (at {x:g} {y:g}) (layer \"{layer}\")\n"
                f"    (effects (font (size 1 1) (thickness 0.15))))")

    o = [f"(footprint \"{name}\" (version 20221018) (generator gen_seismo_carrier)",
         "  (layer \"F.Cu\")",
         "  (descr \"CTS CB3/CB3LV 7.0x5.0 mm SMD clock oscillator, 4 pads, land pattern from CTS 008-0256-0 Rev H\")",
         "  (tags \"oscillator CTS CB3 CB3LV 7050 SMD\")",
         "  (attr smd)",
         text("reference", "REF**", 0, -4.3, "F.SilkS"),
         text("value", name, 0, 4.3, "F.Fab"),
         text("user", "${REFERENCE}", 0, 0, "F.Fab")]
    # Fab: 7.0 x 5.0 body, pin 1 corner chamfered (bottom left).
    fab = [(-3.5, -2.5), (3.5, -2.5), (3.5, 2.5), (-2.5, 2.5), (-3.5, 1.5), (-3.5, -2.5)]
    for (x1, y1), (x2, y2) in zip(fab, fab[1:]):
        o.append(line(x1, y1, x2, y2, "F.Fab", 0.1))
    # Silk: body edges where they clear the pads by 0.2 mm, plus a pin 1 dot.
    sx = px - pw / 2 - 0.2          # 1.44
    sy = py - ph / 2 - 0.2          # 0.90
    for y in (-2.61, 2.61):
        o.append(line(-sx, y, sx, y, "F.SilkS", 0.12))
    for x in (-3.61, 3.61):
        o.append(line(x, -sy, x, sy, "F.SilkS", 0.12))
    o.append("  (fp_circle (center -4.05 2.1) (end -3.85 2.1) (stroke (width 0.12) (type solid)) "
             "(fill solid) (layer \"F.SilkS\"))")
    # Courtyard around pads, body and the pin 1 dot.
    for (x1, y1), (x2, y2) in (((-4.4, -3.4), (3.95, -3.4)), ((3.95, -3.4), (3.95, 3.4)),
                               ((3.95, 3.4), (-4.4, 3.4)), ((-4.4, 3.4), (-4.4, -3.4))):
        o.append(line(x1, y1, x2, y2, "F.CrtYd", 0.05))
    for n, (x, y) in pads.items():
        o.append(f"  (pad \"{n}\" smd rect (at {x:g} {y:g}) (size {pw:g} {ph:g}) "
                 f"(layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))")
    o.append(")")
    return name, "\n".join(o) + "\n"


fp_dir = os.path.join(OUT, "seismo.pretty")
os.makedirs(fp_dir, exist_ok=True)
fp_name, fp_text = kicad_fp_cb3lv()
with open(os.path.join(fp_dir, fp_name + ".kicad_mod"), "w") as f:
    f.write(fp_text)
with open(os.path.join(OUT, "fp-lib-table"), "w") as f:
    f.write("(fp_lib_table\n  (version 7)\n"
            "  (lib (name \"seismo\")(type \"KiCad\")(uri \"${KIPRJMOD}/seismo.pretty\")(options \"\")"
            "(descr \"VSEISMO carrier project footprints\"))\n)\n")
used = {fp.split(":")[1] for _r, _v, fp, _s, _p, _n in PARTS if fp.startswith("seismo:")}
assert used == {fp_name}, f"seismo: footprints used {used}, generated {fp_name}"

print(f"ok: {len(PARTS)} parts, {len(nets)} nets, footprint seismo:{fp_name}")
