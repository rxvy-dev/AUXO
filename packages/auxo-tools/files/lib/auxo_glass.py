"""Liquid Glass panels for Auxo's shell apps (auxo-launcher, auxo-control).

The window is see-through; this paints the glass on it: a frosted fill that's lighter at the
top, a soft specular highlight, and a bright rim that fades from top to bottom. On Hyprland the
compositor blurs whatever is behind (the rice's window rules), which gives the frosted depth."""
import os

from PyQt6.QtCore import QEvent, QPointF, QRectF, Qt, QTimer
from PyQt6.QtGui import QColor, QLinearGradient, QPainter, QPainterPath, QPen, QRadialGradient
from PyQt6.QtWidgets import QApplication, QWidget


def glass_colors(style, light):
    """(fill top, fill bottom, rim top, rim bottom, outline) for a panel style."""
    if style == "windows":                       # Windows 11 acrylic: mostly solid
        base = QColor("#f3f3f3" if light else "#202020")
        top, bottom = QColor(base), QColor(base)
        top.setAlphaF(0.93)
        bottom.setAlphaF(0.96)
        rim = QColor(255, 255, 255, 40 if not light else 160)
        return top, bottom, rim, QColor(rim.red(), rim.green(), rim.blue(), 10), QColor(0, 0, 0, 30 if light else 90)
    if light:
        return (QColor(255, 255, 255, 120), QColor(255, 255, 255, 70), QColor(255, 255, 255, 235),
                QColor(255, 255, 255, 70), QColor(0, 0, 0, 22))
    return (QColor(60, 60, 78, 120), QColor(28, 28, 40, 105), QColor(255, 255, 255, 120),
            QColor(255, 255, 255, 18), QColor(0, 0, 0, 70))


def paint_glass(p, rect, radius, style, light, gloss=True):
    top, bottom, rim_top, rim_bottom, outline = glass_colors(style, light)
    path = QPainterPath()
    path.addRoundedRect(rect, radius, radius)
    g = QLinearGradient(rect.topLeft(), rect.bottomLeft())
    g.setColorAt(0, top)
    g.setColorAt(1, bottom)
    p.fillPath(path, g)
    if gloss and style != "windows":
        r = QRadialGradient(QPointF(rect.left() + rect.width() * 0.18, rect.top()), rect.width() * 0.75)
        r.setColorAt(0, QColor(255, 255, 255, 70 if light else 32))
        r.setColorAt(1, QColor(255, 255, 255, 0))
        p.fillPath(path, r)
    p.setPen(QPen(outline, 1))
    p.drawPath(path)
    inner = QPainterPath()
    inner.addRoundedRect(rect.adjusted(1, 1, -1, -1), radius - 1, radius - 1)
    rg = QLinearGradient(rect.topLeft(), rect.bottomLeft())
    rg.setColorAt(0, rim_top)
    rg.setColorAt(0.5, rim_bottom)
    rg.setColorAt(1, QColor(rim_top.red(), rim_top.green(), rim_top.blue(), rim_top.alpha() // 3))
    p.setPen(QPen(rg, 1.2))
    p.drawPath(inner)


class GlassPanel(QWidget):
    """A frameless, see-through window with a glass panel painted on it. Closes on Esc or when
    you click somewhere else."""

    def __init__(self, title, radius, style, light):
        super().__init__()
        self.radius, self.glass_style, self.light = radius, style, light
        self.setWindowTitle(title)
        self.setWindowFlag(Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.armed = False
        QTimer.singleShot(600, lambda: setattr(self, "armed", True))   # ignore the focus dance while opening

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        paint_glass(p, QRectF(0.5, 0.5, self.width() - 1, self.height() - 1), self.radius, self.glass_style,
                    self.light)
        p.end()

    def keyPressEvent(self, e):
        if e.key() == Qt.Key.Key_Escape:
            self.close()
        else:
            super().keyPressEvent(e)

    def changeEvent(self, e):
        if e.type() == QEvent.Type.ActivationChange and self.armed and not self.isActiveWindow() \
                and not QApplication.activePopupWidget() and not os.environ.get("AUXO_LAUNCHER_STAY"):
            self.close()
        super().changeEvent(e)


def glass_css(light, radius, selector):
    """Qt stylesheet for glass tiles inside a panel (lighter fill + bright top edge)."""
    if light:
        return (f"{selector} {{ background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 rgba(255,255,255,0.62),"
                f" stop:1 rgba(255,255,255,0.36)); border: 1px solid rgba(255,255,255,0.75);"
                f" border-bottom-color: rgba(255,255,255,0.3); border-radius: {radius}px; }}")
    return (f"{selector} {{ background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 rgba(255,255,255,0.14),"
            f" stop:1 rgba(255,255,255,0.06)); border: 1px solid rgba(255,255,255,0.22);"
            f" border-bottom-color: rgba(255,255,255,0.06); border-radius: {radius}px; }}")


def _focused_monitor():
    """(x, y, width, height) of the monitor in use, in layout pixels, or None."""
    import json
    import subprocess
    try:
        if os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"):
            mons = json.loads(subprocess.run(["hyprctl", "monitors", "-j"], capture_output=True, text=True,
                                             timeout=2).stdout or "[]")
            m = next((m for m in mons if m.get("focused")), mons[0] if mons else None)
            if m:
                sc = m.get("scale") or 1
                w, h = m["width"] / sc, m["height"] / sc
                if m.get("transform", 0) % 2:
                    w, h = h, w
                res = m.get("reserved") or [0, 0, 0, 0]          # space the bars take: left, top, right, bottom
                return m["x"], m["y"], w, h, res
        if os.environ.get("SWAYSOCK"):
            outs = json.loads(subprocess.run(["swaymsg", "-t", "get_outputs", "-r"], capture_output=True, text=True,
                                             timeout=2).stdout or "[]")
            o = next((o for o in outs if o.get("focused")), outs[0] if outs else None)
            if o:
                r = o["rect"]
                return r["x"], r["y"], r["width"], r["height"], [0, 0, 0, 0]
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired):
        pass
    return None


def place(widget, app_id, where, margin=12, bar=0):
    """Put a panel where it belongs on the monitor in use: center, top-right, bottom-center or bottom-right.
    `bar` is the height of the bar on that edge (used when the compositor doesn't report it)."""
    import subprocess
    mon = _focused_monitor()
    if not mon:
        return False
    mx, my, mw, mh, res = mon
    w, h = widget.width(), widget.height()
    top = max(res[1], bar if where.startswith("top") else 0)
    bottom = max(res[3], bar if where.startswith("bottom") else 0)
    x = {"center": (mw - w) / 2, "top-right": mw - w - margin, "bottom-center": (mw - w) / 2,
         "bottom-right": mw - w - margin}[where]
    y = {"center": (mh - h) / 2, "top-right": top + margin, "bottom-center": mh - bottom - h - margin,
         "bottom-right": mh - bottom - h - margin}[where]
    x, y = int(mx + x), int(my + y)
    try:
        if os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"):
            subprocess.Popen(["hyprctl", "dispatch", "movewindowpixel", f"exact {x} {y},class:^({app_id})$"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        elif os.environ.get("SWAYSOCK"):
            subprocess.Popen(["swaymsg", f'[app_id="{app_id}"] floating enable, move absolute position {x} {y}'],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            return False
    except OSError:
        return False
    return True
