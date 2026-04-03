"""Main application window for Ants Terminal."""

from PyQt6.QtWidgets import QMainWindow, QWidget, QVBoxLayout
from PyQt6.QtGui import QAction, QKeySequence, QFont
from PyQt6.QtCore import Qt

from app import __version__
from app.config import Config
from app.icon import get_app_icon
from app.themes import get_stylesheet, get_theme, get_theme_names
from app.chat_display import ChatDisplay
from app.input_widget import InputArea


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.config = Config()
        self.setWindowTitle("Ants Terminal")
        self.setWindowIcon(get_app_icon())
        self._restore_geometry()
        self._setup_ui()
        self._setup_menu()
        self._setup_shortcuts()
        self._apply_theme(self.config.get("theme"))
        self._show_welcome()

    def _setup_ui(self):
        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        self.display = ChatDisplay()
        layout.addWidget(self.display, stretch=1)

        self.input_area = InputArea()
        layout.addWidget(self.input_area, stretch=0)

        self.setCentralWidget(central)

        self.statusBar().showMessage(f"Ants Terminal v{__version__}")

        self.input_area.submitted.connect(self._on_submitted)

    def _setup_menu(self):
        menu_bar = self.menuBar()

        # File
        file_menu = menu_bar.addMenu("&File")

        clear_action = QAction("&Clear Chat", self)
        clear_action.setShortcut(QKeySequence("Ctrl+L"))
        clear_action.triggered.connect(self.display.clear_messages)
        file_menu.addAction(clear_action)

        file_menu.addSeparator()

        exit_action = QAction("E&xit", self)
        exit_action.setShortcut(QKeySequence("Ctrl+Q"))
        exit_action.triggered.connect(self.close)
        file_menu.addAction(exit_action)

        # View
        view_menu = menu_bar.addMenu("&View")

        # Themes submenu
        themes_menu = view_menu.addMenu("&Themes")
        for name in get_theme_names():
            action = QAction(name, self)
            action.triggered.connect(lambda checked, n=name: self._apply_theme(n))
            themes_menu.addAction(action)

        view_menu.addSeparator()

        # Font size
        zoom_in = QAction("Zoom &In", self)
        zoom_in.setShortcut(QKeySequence("Ctrl++"))
        zoom_in.triggered.connect(lambda: self._change_font_size(1))
        view_menu.addAction(zoom_in)

        zoom_out = QAction("Zoom &Out", self)
        zoom_out.setShortcut(QKeySequence("Ctrl+-"))
        zoom_out.triggered.connect(lambda: self._change_font_size(-1))
        view_menu.addAction(zoom_out)

        reset_zoom = QAction("&Reset Zoom", self)
        reset_zoom.setShortcut(QKeySequence("Ctrl+0"))
        reset_zoom.triggered.connect(self._reset_font_size)
        view_menu.addAction(reset_zoom)

        view_menu.addSeparator()

        center_action = QAction("&Center Window", self)
        center_action.setShortcut(QKeySequence("Ctrl+Shift+C"))
        center_action.triggered.connect(self._center_window)
        view_menu.addAction(center_action)

    def _setup_shortcuts(self):
        # Ctrl+= as alternate zoom in (for keyboards where + requires Shift)
        alt_zoom_in = QAction(self)
        alt_zoom_in.setShortcut(QKeySequence("Ctrl+="))
        alt_zoom_in.triggered.connect(lambda: self._change_font_size(1))
        self.addAction(alt_zoom_in)

    def _apply_theme(self, theme_name: str):
        self.config.set("theme", theme_name)
        self.setStyleSheet(get_stylesheet(theme_name))
        self.display.set_theme(get_theme(theme_name))
        self.statusBar().showMessage(f"Theme: {theme_name}")

    def _change_font_size(self, delta: int):
        size = self.config.get("font_size") + delta
        size = max(8, min(24, size))
        self.config.set("font_size", size)
        font = self.font()
        font.setPointSize(size)
        self.setFont(font)
        self.statusBar().showMessage(f"Font size: {size}pt")

    def _reset_font_size(self):
        from app.config import DEFAULTS
        size = DEFAULTS["font_size"]
        self.config.set("font_size", size)
        font = self.font()
        font.setPointSize(size)
        self.setFont(font)
        self.statusBar().showMessage(f"Font size reset to {size}pt")

    def _center_window(self):
        screen = self.screen()
        if screen:
            geo = screen.availableGeometry()
            x = geo.x() + (geo.width() - self.width()) // 2
            y = geo.y() + (geo.height() - self.height()) // 2
            self.move(x, y)
            self.statusBar().showMessage("Window centered")

    def _on_submitted(self, text: str, images: list):
        self.display.add_message("You", text, images or None)

    def _show_welcome(self):
        lines = [
            f"Welcome to Ants Terminal v{__version__}",
            "\u2500" * 40,
            "",
            "  Enter        \u2192  Send message",
            "  Shift+Enter  \u2192  New line",
            "  Ctrl+V       \u2192  Paste image",
            "  Ctrl+L       \u2192  Clear chat",
            "  Ctrl++/-     \u2192  Zoom in/out",
            "  Ctrl+Shift+C \u2192  Center window",
            "",
            "  View \u2192 Themes to switch themes.",
        ]
        self.display.add_message("Ants", "\n".join(lines))

    def _restore_geometry(self):
        w = self.config.get("window_width")
        h = self.config.get("window_height")
        self.resize(w, h)
        x = self.config.get("window_x")
        y = self.config.get("window_y")
        if x is not None and y is not None:
            self.move(x, y)

    def closeEvent(self, event):
        self.config.set("window_width", self.width())
        self.config.set("window_height", self.height())
        self.config.set("window_x", self.x())
        self.config.set("window_y", self.y())
        event.accept()
