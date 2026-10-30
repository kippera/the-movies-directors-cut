#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Movies graphics settings: edits Game Files/TheMoviesGfx.ini (colour, FXAA, sharpen, MSAA, movie resolution, text fix)
and Game Files/dxvk.conf (anisotropic filtering, LOD bias).  Colour / FXAA / sharpen apply live while the game runs
(the proxy reloads the ini), the rest need a game restart (marked 'restart')."""
import os, re, sys
from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtWidgets import (QApplication, QWidget, QVBoxLayout, QHBoxLayout, QFormLayout, QGroupBox, QLabel,
                             QSlider, QCheckBox, QComboBox, QPushButton)

GAME = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()  # usage: gfx_settings.py [folder with MoviesSE.exe]
INI = os.path.join(GAME, "TheMoviesGfx.ini")
DXVK = os.path.join(GAME, "dxvk.conf")


def ini_get(key, default):
    try:
        for line in open(INI, errors="replace"):
            m = re.match(r"\s*%s\s*=\s*(.*)$" % re.escape(key), line)
            if m:
                return m.group(1).strip()
    except OSError:
        pass
    return default


def ini_set(key, value):
    lines = open(INI, errors="replace").read().split("\n") if os.path.exists(INI) else ["[Gfx]"]
    done = False
    for i, line in enumerate(lines):
        if re.match(r"\s*%s\s*=" % re.escape(key), line):
            lines[i] = "%s=%s" % (key, value)
            done = True
            break
    if not done:
        idx = next((i for i, l in enumerate(lines) if l.strip().lower() == "[gfx]"), -1)
        lines.insert(idx + 1 if idx >= 0 else len(lines), "%s=%s" % (key, value))
    tmp = INI + ".tmp"
    open(tmp, "w").write("\n".join(lines))
    os.replace(tmp, INI)


def dxvk_get(key, default):
    try:
        for line in open(DXVK, errors="replace"):
            m = re.match(r"\s*%s\s*=\s*(.*)$" % re.escape(key), line)
            if m:
                return m.group(1).strip()
    except OSError:
        pass
    return default


def dxvk_set(key, value):
    lines = open(DXVK, errors="replace").read().split("\n") if os.path.exists(DXVK) else []
    lines = [l for l in lines if l.strip() and not re.match(r"\s*%s\s*=" % re.escape(key), l)]
    if value is not None:
        lines.append("%s = %s" % (key, value))
    open(DXVK, "w").write("\n".join(lines) + "\n")


class Row:
    """slider with a value label, mapped int <-> float"""

    def __init__(self, form, label, key, lo, hi, step, default, live=True, fmt="%.2f"):
        self.key, self.lo, self.step, self.fmt, self.default = key, lo, step, fmt, default
        self.s = QSlider(Qt.Orientation.Horizontal)
        self.s.setRange(0, int(round((hi - lo) / step)))
        self.val = QLabel()
        self.val.setMinimumWidth(48)
        try:
            cur = float(ini_get(key, default))
        except ValueError:
            cur = default
        self.s.setValue(int(round((cur - lo) / step)))
        self.s.valueChanged.connect(self.changed)
        box = QHBoxLayout()
        box.addWidget(self.s)
        box.addWidget(self.val)
        form.addRow(label + ("" if live else "  (restart)"), box)
        self.refresh()

    def value(self):
        return self.lo + self.s.value() * self.step

    def refresh(self):
        self.val.setText(self.fmt % self.value())

    def changed(self, _):
        self.refresh()
        ini_set(self.key, self.fmt % self.value())

    def reset(self):
        self.s.setValue(int(round((self.default - self.lo) / self.step)))


class Win(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("The Movies - graphics settings")
        root = QVBoxLayout(self)

        g = QGroupBox("Colour (live)")
        f = QFormLayout(g)
        self.en = QCheckBox("Post-processing enabled")
        self.en.setChecked(ini_get("Enabled", "1") != "0")
        self.en.toggled.connect(lambda v: ini_set("Enabled", "1" if v else "0"))
        f.addRow(self.en)
        self.rows = [Row(f, "Brightness", "Brightness", -0.5, 0.5, 0.01, 0.0),
                     Row(f, "Contrast", "Contrast", 0.5, 2.0, 0.01, 1.0),
                     Row(f, "Gamma", "Gamma", 0.3, 3.0, 0.01, 0.88 if False else 1.0),
                     Row(f, "Saturation", "Saturation", 0.0, 2.0, 0.01, 1.0)]
        b = QPushButton("Reset colour")
        b.clicked.connect(lambda: [r.reset() for r in self.rows])
        f.addRow(b)
        root.addWidget(g)

        g = QGroupBox("Anti-aliasing / sharpness")
        f = QFormLayout(g)
        self.fxaa = QCheckBox("FXAA (post-process, live)")
        self.fxaa.setChecked(ini_get("FXAA", "0") not in ("0", ""))
        self.fxaa.toggled.connect(lambda v: ini_set("FXAA", "1" if v else "0"))
        f.addRow(self.fxaa)
        self.rows.append(Row(f, "Sharpen", "Sharpen", 0.0, 1.0, 0.05, 0.0))
        self.msaa = QComboBox()
        self.msaa.addItems(["Off", "2x", "4x", "8x"])
        cur = {"0": 0, "2": 1, "4": 2, "8": 3}.get(ini_get("MSAA", "0"), 0)
        self.msaa.setCurrentIndex(cur)
        self.msaa.currentIndexChanged.connect(lambda i: ini_set("MSAA", ["0", "2", "4", "8"][i]))
        f.addRow("MSAA  (restart, experimental)", self.msaa)
        root.addWidget(g)

        g = QGroupBox("Texture filtering (restart)")
        f = QFormLayout(g)
        self.af = QComboBox()
        self.af.addItems(["Game default", "2x", "4x", "8x", "16x"])
        cur = {"": 0, "2": 1, "4": 2, "8": 3, "16": 4}.get(dxvk_get("d3d9.samplerAnisotropy", ""), 0)
        self.af.setCurrentIndex(cur)
        self.af.currentIndexChanged.connect(lambda i: dxvk_set("d3d9.samplerAnisotropy", [None, "2", "4", "8", "16"][i]))
        f.addRow("Anisotropic filtering", self.af)
        self.lod = QSlider(Qt.Orientation.Horizontal)
        self.lod.setRange(-20, 10)
        try:
            self.lod.setValue(int(round(float(dxvk_get("d3d9.samplerLodBias", "0")) * 10)))
        except ValueError:
            self.lod.setValue(0)
        self.lodv = QLabel("%.1f" % (self.lod.value() / 10))
        self.lod.valueChanged.connect(self.lod_changed)
        box = QHBoxLayout()
        box.addWidget(self.lod)
        box.addWidget(self.lodv)
        f.addRow("Texture LOD bias (negative = sharper)", box)
        root.addWidget(g)

        g = QGroupBox("Movies and text")
        f = QFormLayout(g)
        self.mres = QComboBox()
        self.mres.addItems(["512 (stock)", "1024", "2048"])
        cur = {"512": 0, "1024": 1, "2048": 2}.get(ini_get("MovieRes", "1024"), 1)
        self.mres.setCurrentIndex(cur)
        self.mres.currentIndexChanged.connect(lambda i: ini_set("MovieRes", ["512", "1024", "2048"][i]))
        f.addRow("Movie playback resolution (restart)", self.mres)
        self.td = QCheckBox("Fix clipped letter tails in text (restart)")
        self.td.setChecked(ini_get("TextDescent", "1") not in ("0", ""))
        self.td.toggled.connect(lambda v: ini_set("TextDescent", "1" if v else "0"))
        f.addRow(self.td)
        root.addWidget(g)

        root.addWidget(QLabel("Settings are saved as you change them. 'restart' items apply the next time the game starts."))

    def lod_changed(self, v):
        self.lodv.setText("%.1f" % (v / 10))
        dxvk_set("d3d9.samplerLodBias", None if v == 0 else "%.1f" % (v / 10))


if __name__ == "__main__":
    app = QApplication(sys.argv)
    w = Win()
    w.show()
    sys.exit(app.exec())
