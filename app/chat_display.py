"""Chat display widget for rendering messages and images."""

import uuid
from dataclasses import dataclass, field
from datetime import datetime

from PyQt6.QtWidgets import QTextBrowser
from PyQt6.QtCore import QUrl
from PyQt6.QtGui import QImage, QPixmap, QTextDocument


@dataclass
class Message:
    sender: str
    text: str = ""
    images: list = field(default_factory=list)
    timestamp: datetime = field(default_factory=datetime.now)
    id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])


class ChatDisplay(QTextBrowser):
    """Scrollable display area for chat messages with image support."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setOpenExternalLinks(True)
        self.setReadOnly(True)
        self._messages: list[Message] = []
        self._theme: dict = {}

    def set_theme(self, theme: dict):
        self._theme = theme
        self._render_all()

    def add_message(self, sender: str, text: str = "", images: list[QImage] | None = None):
        msg = Message(sender=sender, text=text, images=images or [])
        self._messages.append(msg)
        self._register_images(msg)
        self._render_message(msg)
        sb = self.verticalScrollBar()
        sb.setValue(sb.maximum())

    def clear_messages(self):
        self._messages.clear()
        self.clear()

    def _register_images(self, msg: Message):
        doc = self.document()
        for i, img in enumerate(msg.images):
            name = f"{msg.id}_{i}"
            pixmap = QPixmap.fromImage(img)
            doc.addResource(QTextDocument.ResourceType.ImageResource, QUrl(name), pixmap)

    def _render_all(self):
        self.clear()
        for msg in self._messages:
            self._register_images(msg)
        for msg in self._messages:
            self._render_message(msg)

    def _render_message(self, msg: Message):
        t = self._theme
        user_color = t.get("user_accent", "#89b4fa")
        sys_color = t.get("system_accent", "#a6e3a1")
        muted = t.get("text_muted", "#6c7086")
        text_color = t.get("text_primary", "#cdd6f4")
        sep_color = t.get("separator", "#313244")

        sender_color = user_color if msg.sender == "You" else sys_color
        time_str = msg.timestamp.strftime("%H:%M:%S")

        html = (
            f'<div style="margin: 2px 12px; padding: 6px 0;">'
            f'<span style="font-weight: bold; color: {sender_color};">{_esc(msg.sender)}</span>'
            f'&nbsp;&nbsp;'
            f'<span style="font-size: 0.85em; color: {muted};">{time_str}</span>'
        )

        if msg.text:
            text_html = _esc(msg.text).replace("\n", "<br/>")
            html += f'<div style="margin-top: 4px; color: {text_color};">{text_html}</div>'

        for i, img in enumerate(msg.images):
            name = f"{msg.id}_{i}"
            w = min(img.width(), 500)
            html += (
                f'<div style="margin-top: 6px;">'
                f'<img src="{name}" width="{w}"/>'
                f'</div>'
            )

        html += "</div>"
        html += f'<hr style="border: none; border-top: 1px solid {sep_color}; margin: 0 12px;"/>'

        self.append(html)


def _esc(text: str) -> str:
    """Escape HTML special characters."""
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )
