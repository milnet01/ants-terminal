"""Theme definitions and stylesheet generation for Ants Terminal."""

THEMES = {
    "Dark": {
        "bg_primary": "#1e1e2e",
        "bg_secondary": "#181825",
        "bg_input": "#313244",
        "bg_surface": "#313244",
        "bg_hover": "#45475a",
        "text_primary": "#cdd6f4",
        "text_secondary": "#a6adc8",
        "text_muted": "#6c7086",
        "accent": "#89b4fa",
        "user_accent": "#89b4fa",
        "system_accent": "#a6e3a1",
        "border": "#45475a",
        "separator": "#313244",
        "scrollbar_bg": "#181825",
        "scrollbar_handle": "#585b70",
        "selection_bg": "#89b4fa",
        "selection_text": "#1e1e2e",
    },
    "Light": {
        "bg_primary": "#ffffff",
        "bg_secondary": "#f5f5f5",
        "bg_input": "#ffffff",
        "bg_surface": "#e8e8e8",
        "bg_hover": "#d0d0d0",
        "text_primary": "#1e1e2e",
        "text_secondary": "#4a4a5a",
        "text_muted": "#8888a0",
        "accent": "#1a73e8",
        "user_accent": "#1a73e8",
        "system_accent": "#0d7a3e",
        "border": "#d0d0d0",
        "separator": "#e0e0e0",
        "scrollbar_bg": "#f0f0f0",
        "scrollbar_handle": "#c0c0c0",
        "selection_bg": "#1a73e8",
        "selection_text": "#ffffff",
    },
    "Nord": {
        "bg_primary": "#2e3440",
        "bg_secondary": "#272c36",
        "bg_input": "#3b4252",
        "bg_surface": "#3b4252",
        "bg_hover": "#434c5e",
        "text_primary": "#eceff4",
        "text_secondary": "#d8dee9",
        "text_muted": "#7b88a1",
        "accent": "#88c0d0",
        "user_accent": "#88c0d0",
        "system_accent": "#a3be8c",
        "border": "#434c5e",
        "separator": "#3b4252",
        "scrollbar_bg": "#272c36",
        "scrollbar_handle": "#4c566a",
        "selection_bg": "#88c0d0",
        "selection_text": "#2e3440",
    },
    "Dracula": {
        "bg_primary": "#282a36",
        "bg_secondary": "#21222c",
        "bg_input": "#44475a",
        "bg_surface": "#44475a",
        "bg_hover": "#6272a4",
        "text_primary": "#f8f8f2",
        "text_secondary": "#bfbfbf",
        "text_muted": "#6272a4",
        "accent": "#bd93f9",
        "user_accent": "#bd93f9",
        "system_accent": "#50fa7b",
        "border": "#44475a",
        "separator": "#44475a",
        "scrollbar_bg": "#21222c",
        "scrollbar_handle": "#6272a4",
        "selection_bg": "#bd93f9",
        "selection_text": "#282a36",
    },
    "Monokai": {
        "bg_primary": "#272822",
        "bg_secondary": "#1e1f1c",
        "bg_input": "#3e3d32",
        "bg_surface": "#3e3d32",
        "bg_hover": "#4e4d42",
        "text_primary": "#f8f8f2",
        "text_secondary": "#c0c0b0",
        "text_muted": "#75715e",
        "accent": "#66d9ef",
        "user_accent": "#66d9ef",
        "system_accent": "#a6e22e",
        "border": "#3e3d32",
        "separator": "#3e3d32",
        "scrollbar_bg": "#1e1f1c",
        "scrollbar_handle": "#75715e",
        "selection_bg": "#66d9ef",
        "selection_text": "#272822",
    },
    "Solarized Dark": {
        "bg_primary": "#002b36",
        "bg_secondary": "#001e27",
        "bg_input": "#073642",
        "bg_surface": "#073642",
        "bg_hover": "#0a4a5a",
        "text_primary": "#839496",
        "text_secondary": "#93a1a1",
        "text_muted": "#586e75",
        "accent": "#268bd2",
        "user_accent": "#268bd2",
        "system_accent": "#859900",
        "border": "#073642",
        "separator": "#073642",
        "scrollbar_bg": "#001e27",
        "scrollbar_handle": "#586e75",
        "selection_bg": "#268bd2",
        "selection_text": "#fdf6e3",
    },
    "Gruvbox": {
        "bg_primary": "#282828",
        "bg_secondary": "#1d2021",
        "bg_input": "#3c3836",
        "bg_surface": "#3c3836",
        "bg_hover": "#504945",
        "text_primary": "#ebdbb2",
        "text_secondary": "#d5c4a1",
        "text_muted": "#928374",
        "accent": "#fabd2f",
        "user_accent": "#fabd2f",
        "system_accent": "#b8bb26",
        "border": "#504945",
        "separator": "#3c3836",
        "scrollbar_bg": "#1d2021",
        "scrollbar_handle": "#665c54",
        "selection_bg": "#fabd2f",
        "selection_text": "#282828",
    },
}


def get_theme_names():
    """Return list of available theme names."""
    return list(THEMES.keys())


def get_theme(name):
    """Return theme dict by name, falling back to Dark."""
    return THEMES.get(name, THEMES["Dark"])


def get_stylesheet(theme_name):
    """Generate a complete Qt stylesheet for the given theme."""
    t = get_theme(theme_name)
    return f"""
        QMainWindow {{
            background-color: {t['bg_primary']};
        }}

        /* Menu Bar */
        QMenuBar {{
            background-color: {t['bg_secondary']};
            color: {t['text_primary']};
            border-bottom: 1px solid {t['border']};
            padding: 2px 4px;
            spacing: 2px;
        }}
        QMenuBar::item {{
            padding: 4px 8px;
            border-radius: 4px;
        }}
        QMenuBar::item:selected {{
            background-color: {t['bg_hover']};
        }}

        /* Dropdown Menus */
        QMenu {{
            background-color: {t['bg_secondary']};
            color: {t['text_primary']};
            border: 1px solid {t['border']};
            border-radius: 6px;
            padding: 4px;
        }}
        QMenu::item {{
            padding: 6px 24px 6px 12px;
            border-radius: 4px;
        }}
        QMenu::item:selected {{
            background-color: {t['accent']};
            color: {t['selection_text']};
        }}
        QMenu::separator {{
            height: 1px;
            background-color: {t['border']};
            margin: 4px 8px;
        }}

        /* Chat Display */
        QTextBrowser {{
            background-color: {t['bg_primary']};
            color: {t['text_primary']};
            border: none;
            padding: 0px;
            selection-background-color: {t['selection_bg']};
            selection-color: {t['selection_text']};
        }}

        /* Input Field */
        QTextEdit {{
            background-color: {t['bg_input']};
            color: {t['text_primary']};
            border: 2px solid {t['border']};
            border-radius: 10px;
            padding: 8px 12px;
            selection-background-color: {t['selection_bg']};
            selection-color: {t['selection_text']};
        }}
        QTextEdit:focus {{
            border: 2px solid {t['accent']};
        }}

        /* Image Preview Strip */
        #imagePreview {{
            background-color: {t['bg_surface']};
            border: 1px solid {t['border']};
            border-radius: 8px;
        }}
        #imagePreview QLabel {{
            color: {t['text_secondary']};
        }}
        #imagePreview QPushButton {{
            background-color: transparent;
            color: {t['text_muted']};
            border: none;
            font-size: 16px;
            font-weight: bold;
        }}
        #imagePreview QPushButton:hover {{
            color: {t['accent']};
        }}

        /* Scrollbars */
        QScrollBar:vertical {{
            background-color: {t['scrollbar_bg']};
            width: 10px;
            border-radius: 5px;
            margin: 0px;
        }}
        QScrollBar::handle:vertical {{
            background-color: {t['scrollbar_handle']};
            border-radius: 5px;
            min-height: 30px;
        }}
        QScrollBar::handle:vertical:hover {{
            background-color: {t['bg_hover']};
        }}
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical {{
            height: 0px;
        }}
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {{
            background: none;
        }}

        /* Horizontal Scrollbar */
        QScrollBar:horizontal {{
            background-color: {t['scrollbar_bg']};
            height: 10px;
            border-radius: 5px;
        }}
        QScrollBar::handle:horizontal {{
            background-color: {t['scrollbar_handle']};
            border-radius: 5px;
            min-width: 30px;
        }}
        QScrollBar::handle:horizontal:hover {{
            background-color: {t['bg_hover']};
        }}
        QScrollBar::add-line:horizontal,
        QScrollBar::sub-line:horizontal {{
            width: 0px;
        }}
        QScrollBar::add-page:horizontal,
        QScrollBar::sub-page:horizontal {{
            background: none;
        }}

        /* Status Bar */
        QStatusBar {{
            background-color: {t['bg_secondary']};
            color: {t['text_muted']};
            border-top: 1px solid {t['border']};
            padding: 2px 8px;
            font-size: 12px;
        }}

        /* General Labels */
        QLabel {{
            color: {t['text_primary']};
        }}

        /* Buttons */
        QPushButton {{
            background-color: {t['bg_surface']};
            color: {t['text_primary']};
            border: 1px solid {t['border']};
            border-radius: 6px;
            padding: 6px 14px;
        }}
        QPushButton:hover {{
            background-color: {t['accent']};
            color: {t['selection_text']};
            border-color: {t['accent']};
        }}
        QPushButton:pressed {{
            background-color: {t['bg_hover']};
        }}

        /* Tooltips */
        QToolTip {{
            background-color: {t['bg_surface']};
            color: {t['text_primary']};
            border: 1px solid {t['border']};
            border-radius: 4px;
            padding: 4px 8px;
        }}
    """
