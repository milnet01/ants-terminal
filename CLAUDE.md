# Ants Terminal

A modern PyQt6 chat terminal with themes, image paste support, and extensible architecture.

## Tech Stack

- **Python 3.13+** with **PyQt6** for the GUI
- **Pillow** for image processing
- Fusion Qt style with custom QSS theming

## Project Structure

```
├── main.py                # Entry point
├── requirements.txt       # Python dependencies
├── app/
│   ├── __init__.py        # Package init, version
│   ├── config.py          # Persistent JSON config (~/.config/ants-terminal/)
│   ├── themes.py          # Theme definitions + QSS stylesheet generator
│   ├── input_widget.py    # InputWidget, ImagePreview, InputArea
│   ├── chat_display.py    # ChatDisplay, Message dataclass
│   └── main_window.py     # MainWindow with menus, shortcuts, layout
├── STANDARDS.md           # Development standards
└── RULES.md               # Project rules
```

## Running

```bash
python3 main.py
```

## Architecture

- **Config** persists to `~/.config/ants-terminal/config.json`
- **Themes** are defined as dicts in `themes.py`; `get_stylesheet()` generates QSS
- **ChatDisplay** stores messages as `Message` dataclass instances; re-renders all on theme change
- **InputArea** composites `ImagePreview` + `InputWidget`; emits `submitted(text, images)`
- **MainWindow** wires everything together

## Conventions

- Signals/slots for all inter-widget communication
- Theme colors referenced by semantic name (e.g., `user_accent`, not `blue`)
- Images stored as QImage in Message; registered as QTextDocument resources for display
- No business logic in widgets — keep them as pure UI

## Adding a New Theme

Add a new entry to `THEMES` dict in `app/themes.py` with all required color keys. It will automatically appear in the View > Themes menu.

## Adding New Features

1. Create a new module in `app/` if the feature is self-contained
2. Wire it into `MainWindow` via signals/slots
3. Update this file and STANDARDS.md if architectural patterns change
