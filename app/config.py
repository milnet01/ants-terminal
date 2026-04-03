"""Persistent configuration for Ants Terminal."""

import json
import os
from pathlib import Path


CONFIG_DIR = Path.home() / ".config" / "ants-terminal"
CONFIG_FILE = CONFIG_DIR / "config.json"

DEFAULTS = {
    "theme": "Dark",
    "font_size": 11,
    "font_family": "Monospace",
    "window_width": 900,
    "window_height": 700,
    "window_x": None,
    "window_y": None,
}

# Validation: (type, min, max) — None means no bound
_VALIDATORS = {
    "theme": (str, None, None),
    "font_size": (int, 8, 48),
    "font_family": (str, None, None),
    "window_width": (int, 200, 10000),
    "window_height": (int, 200, 10000),
    "window_x": ((int, type(None)), -10000, 10000),
    "window_y": ((int, type(None)), -10000, 10000),
}


def _validate(key: str, value) -> bool:
    """Return True if value is acceptable for the given config key."""
    spec = _VALIDATORS.get(key)
    if spec is None:
        return False  # reject unknown keys
    expected_type, lo, hi = spec
    if not isinstance(value, expected_type):
        return False
    if value is None:
        return True
    if lo is not None and isinstance(value, (int, float)) and value < lo:
        return False
    if hi is not None and isinstance(value, (int, float)) and value > hi:
        return False
    if isinstance(value, str) and len(value) > 256:
        return False
    return True


class Config:
    """Manages persistent app configuration stored as JSON."""

    def __init__(self):
        self._data = dict(DEFAULTS)
        self._load()

    def _load(self):
        try:
            if CONFIG_FILE.exists():
                with open(CONFIG_FILE) as f:
                    loaded = json.load(f)
                if not isinstance(loaded, dict):
                    return
                for key in DEFAULTS:
                    if key in loaded and _validate(key, loaded[key]):
                        self._data[key] = loaded[key]
        except (json.JSONDecodeError, OSError, ValueError):
            pass

    def save(self):
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        os.chmod(CONFIG_DIR, 0o700)
        with open(CONFIG_FILE, "w") as f:
            json.dump(self._data, f, indent=2)
        os.chmod(CONFIG_FILE, 0o600)

    def get(self, key, default=None):
        val = self._data.get(key)
        if val is None:
            return DEFAULTS.get(key, default)
        return val

    def set(self, key, value):
        if _validate(key, value):
            self._data[key] = value
            self.save()
