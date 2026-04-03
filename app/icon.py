"""Programmatic icon generation for Ants Terminal."""

from PyQt6.QtGui import (
    QPixmap, QPainter, QColor, QPainterPath, QPen, QBrush,
    QLinearGradient, QIcon,
)
from PyQt6.QtCore import Qt, QRectF, QPointF


def create_app_icon(size: int = 256) -> QPixmap:
    """Generate the Ants Terminal icon at the given size."""
    pixmap = QPixmap(size, size)
    pixmap.fill(Qt.GlobalColor.transparent)

    p = QPainter(pixmap)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)

    s = size / 256.0

    # Background rounded square with gradient
    bg_path = QPainterPath()
    bg_path.addRoundedRect(QRectF(8 * s, 8 * s, 240 * s, 240 * s), 48 * s, 48 * s)
    gradient = QLinearGradient(0, 0, size, size)
    gradient.setColorAt(0, QColor("#1e1e2e"))
    gradient.setColorAt(1, QColor("#14141f"))
    p.fillPath(bg_path, QBrush(gradient))

    ant = QColor("#89b4fa")

    # --- Antennae ---
    pen = QPen(ant)
    pen.setWidthF(6 * s)
    pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    p.setPen(pen)
    p.setBrush(Qt.BrushStyle.NoBrush)

    left_ant = QPainterPath()
    left_ant.moveTo(114 * s, 66 * s)
    left_ant.quadTo(92 * s, 32 * s, 72 * s, 24 * s)
    p.drawPath(left_ant)

    right_ant = QPainterPath()
    right_ant.moveTo(142 * s, 66 * s)
    right_ant.quadTo(164 * s, 32 * s, 184 * s, 24 * s)
    p.drawPath(right_ant)

    # Antenna tips
    p.setPen(Qt.PenStyle.NoPen)
    p.setBrush(ant)
    p.drawEllipse(QPointF(72 * s, 24 * s), 5 * s, 5 * s)
    p.drawEllipse(QPointF(184 * s, 24 * s), 5 * s, 5 * s)

    # --- Body ---
    p.setPen(Qt.PenStyle.NoPen)
    p.setBrush(ant)

    # Head
    p.drawEllipse(QPointF(128 * s, 80 * s), 25 * s, 22 * s)
    # Thorax
    p.drawEllipse(QPointF(128 * s, 128 * s), 20 * s, 24 * s)
    # Abdomen
    abdomen_grad = QLinearGradient(
        QPointF(128 * s, 150 * s), QPointF(128 * s, 216 * s)
    )
    abdomen_grad.setColorAt(0, ant)
    abdomen_grad.setColorAt(1, QColor("#6c98d4"))
    p.setBrush(QBrush(abdomen_grad))
    p.drawEllipse(QPointF(128 * s, 184 * s), 32 * s, 36 * s)

    # --- Legs ---
    leg_pen = QPen(ant)
    leg_pen.setWidthF(5 * s)
    leg_pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    p.setPen(leg_pen)
    p.setBrush(Qt.BrushStyle.NoBrush)

    legs = [
        # (body_x, body_y, tip_x, tip_y) — top pair
        (112, 118, 64, 94),
        (144, 118, 192, 94),
        # middle pair
        (108, 136, 58, 142),
        (148, 136, 198, 142),
        # bottom pair
        (110, 162, 62, 190),
        (146, 162, 194, 190),
    ]
    for bx, by, tx, ty in legs:
        p.drawLine(QPointF(bx * s, by * s), QPointF(tx * s, ty * s))

    # --- Eyes ---
    p.setPen(Qt.PenStyle.NoPen)
    p.setBrush(QColor("#1e1e2e"))
    p.drawEllipse(QPointF(117 * s, 75 * s), 6 * s, 6 * s)
    p.drawEllipse(QPointF(139 * s, 75 * s), 6 * s, 6 * s)

    # Eye highlights
    p.setBrush(QColor("#ffffff"))
    p.drawEllipse(QPointF(119 * s, 73 * s), 2.5 * s, 2.5 * s)
    p.drawEllipse(QPointF(141 * s, 73 * s), 2.5 * s, 2.5 * s)

    # --- Terminal cursor (subtle, on abdomen) ---
    cursor_pen = QPen(QColor("#1e1e2e"))
    cursor_pen.setWidthF(3 * s)
    cursor_pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    p.setPen(cursor_pen)
    # >_ prompt
    p.drawLine(QPointF(114 * s, 176 * s), QPointF(124 * s, 184 * s))
    p.drawLine(QPointF(114 * s, 192 * s), QPointF(124 * s, 184 * s))
    p.drawLine(QPointF(128 * s, 192 * s), QPointF(142 * s, 192 * s))

    p.end()
    return pixmap


def get_app_icon() -> QIcon:
    """Return a QIcon with multiple sizes for the app."""
    icon = QIcon()
    for size in (16, 24, 32, 48, 64, 128, 256):
        icon.addPixmap(create_app_icon(size))
    return icon
