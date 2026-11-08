# Feature: the AI Assistant's chat history has a size cap (ANTS-5200)

## Problem

MainWindow builds the AI Assistant dialog once and keeps it.
`AiDialog::appendMessage` appends every message to the chat view, and
`AiDialog::resetTransient` leaves the history alone on purpose. With no
limit, a long session holds every message in memory until the app quits.

The requests sent to the model carry only the system prompt and the
newest message (`LlmClient`), so the chat view is the only copy that
grows.

## Contract

`AiDialog::trimHistory(doc, maxChars)` drops the oldest blocks of the
chat document once it holds more than `maxChars` characters.
`appendMessage` calls it with `AiDialog::kMaxHistoryChars` after each
message. It never runs mid-stream, because `onLlmFinished` deletes the
streamed reply by a remembered character position.

- **INV-1** A document over the cap is trimmed to at most the cap.
- **INV-2** The newest block survives whole, and the oldest is gone.
- **INV-3** A document at or under the cap is left unchanged.
- **INV-4** `appendMessage` calls `trimHistory` with `kMaxHistoryChars`.

## Tests

`test_ai_chat_history_cap.cpp`: INV-1 to INV-3 drive the helper on a
`QTextDocument`; INV-4 reads `src/aidialog.cpp`.
