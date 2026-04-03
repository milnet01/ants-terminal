"""Input widgets: text input with Shift+Enter, image preview, and composite input area."""

from PyQt6.QtWidgets import QTextEdit, QWidget, QHBoxLayout, QVBoxLayout, QLabel, QPushButton
from PyQt6.QtCore import pyqtSignal, Qt
from PyQt6.QtGui import QImage, QPixmap, QKeyEvent


class InputWidget(QTextEdit):
    """Text input that emits enter_pressed on Enter, inserts newline on Shift+Enter."""

    enter_pressed = pyqtSignal()
    image_pasted = pyqtSignal(QImage)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setPlaceholderText("Type a message... (Shift+Enter for new line)")
        self.setAcceptRichText(False)
        self.setMinimumHeight(50)
        self.setMaximumHeight(160)

    def keyPressEvent(self, event: QKeyEvent):
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            if event.modifiers() & Qt.KeyboardModifier.ShiftModifier:
                super().keyPressEvent(event)
            else:
                self.enter_pressed.emit()
        else:
            super().keyPressEvent(event)

    def insertFromMimeData(self, source):
        if source.hasImage():
            data = source.imageData()
            if isinstance(data, QImage) and not data.isNull():
                self.image_pasted.emit(data)
                return
            if data is not None:
                img = QImage(data)
                if not img.isNull():
                    self.image_pasted.emit(img)
                    return
        super().insertFromMimeData(source)


class ImagePreview(QWidget):
    """Shows a thumbnail preview of a pasted image above the input."""

    removed = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("imagePreview")
        self.setVisible(False)
        self._image = None
        self._setup_ui()

    def _setup_ui(self):
        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)
        layout.setSpacing(8)

        self._thumbnail = QLabel()
        self._thumbnail.setFixedSize(48, 48)
        self._thumbnail.setScaledContents(False)
        layout.addWidget(self._thumbnail)

        self._label = QLabel("Pasted image")
        layout.addWidget(self._label)

        layout.addStretch()

        self._remove_btn = QPushButton("\u2715")
        self._remove_btn.setFixedSize(28, 28)
        self._remove_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._remove_btn.setToolTip("Remove image")
        self._remove_btn.clicked.connect(self.clear_image)
        layout.addWidget(self._remove_btn)

    def set_image(self, image: QImage):
        self._image = image
        pixmap = QPixmap.fromImage(image).scaled(
            48, 48,
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        self._thumbnail.setPixmap(pixmap)
        self._label.setText(f"Pasted image ({image.width()}\u00d7{image.height()})")
        self.setVisible(True)

    def clear_image(self):
        self._image = None
        self._thumbnail.clear()
        self.setVisible(False)
        self.removed.emit()

    def get_image(self):
        return self._image


class InputArea(QWidget):
    """Composite widget: image preview + text input. Emits submitted(text, images)."""

    submitted = pyqtSignal(str, list)

    def __init__(self, parent=None):
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 4, 10, 10)
        layout.setSpacing(4)

        self.image_preview = ImagePreview()
        layout.addWidget(self.image_preview)

        self.input = InputWidget()
        layout.addWidget(self.input)

        self.input.enter_pressed.connect(self._on_enter)
        self.input.image_pasted.connect(self.image_preview.set_image)

    def _on_enter(self):
        text = self.input.toPlainText().strip()
        images = []
        img = self.image_preview.get_image()
        if img is not None:
            images.append(img)
        if text or images:
            self.submitted.emit(text, images)
            self.input.clear()
            self.image_preview.clear_image()

    def focus_input(self):
        self.input.setFocus()
