# Ants Terminal

A modern, themeable chat terminal built with Python and PyQt6.

## Features

- **7 Built-in Themes** — Dark, Light, Nord, Dracula, Monokai, Solarized Dark, Gruvbox
- **Image Paste Support** — Paste images from clipboard with preview before sending
- **Smart Input** — Shift+Enter for new lines, Enter to submit
- **Persistent Config** — Window size, position, theme, and font size saved between sessions
- **Zoom Controls** — Ctrl+/- to adjust font size

## Screenshots

*Coming soon*

## Installation

```bash
# Clone the repository
git clone https://github.com/milnet01/ants-terminal.git
cd ants-terminal

# Install dependencies
pip install -r requirements.txt

# Run
python3 main.py
```

## Requirements

- Python 3.11+
- PyQt6 >= 6.6
- Pillow >= 10.0

## Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| Enter | Send message |
| Shift+Enter | New line |
| Ctrl+V | Paste image |
| Ctrl+L | Clear chat |
| Ctrl++ | Zoom in |
| Ctrl+- | Zoom out |
| Ctrl+0 | Reset zoom |
| Ctrl+Shift+C | Center window |
| Ctrl+Q | Quit |

## Adding Themes

Add a new entry to the `THEMES` dict in `app/themes.py` with all required color keys. It will automatically appear in the View > Themes menu.

## License

MIT
