#!/usr/bin/env python3
"""Ants Terminal - A modern terminal with themes, image support, and more."""

import sys

from PyQt6.QtWidgets import QApplication
from PyQt6.QtGui import QFont

from app.icon import get_app_icon
from app.main_window import MainWindow


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("Ants Terminal")
    app.setStyle("Fusion")

    app.setWindowIcon(get_app_icon())

    font = QFont("Monospace", 11)
    font.setStyleHint(QFont.StyleHint.Monospace)
    app.setFont(font)

    window = MainWindow()
    window.show()
    window.input_area.focus_input()

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
