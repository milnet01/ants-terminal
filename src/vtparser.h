#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

// A single action the parser emits for the grid to execute
struct VtAction {
    enum Type {
        Print,          // Regular character to display
        Execute,        // C0 control char (BEL, BS, HT, LF, CR, etc.)
        CsiDispatch,    // CSI sequence complete (cursor move, erase, SGR, etc.)
        EscDispatch,    // ESC sequence complete
        OscEnd,         // OSC sequence complete (window title, etc.)
    };

    Type type;
    uint32_t codepoint = 0;       // For Print
    char controlChar = 0;         // For Execute
    char finalChar = 0;           // For CSI/ESC dispatch
    std::vector<int> params;      // CSI parameters
    std::string intermediate;     // Intermediate bytes
    std::string oscString;        // OSC payload
};

// VT100/xterm escape sequence parser — state machine based on
// Paul Williams' DEC-compatible parser model.
class VtParser {
public:
    using ActionCallback = std::function<void(const VtAction &)>;

    explicit VtParser(ActionCallback callback);

    void feed(const char *data, int length);

private:
    enum State {
        Ground,
        Escape,
        EscapeIntermediate,
        CsiEntry,
        CsiParam,
        CsiIntermediate,
        OscString,
        // Simplified: DCS/SOS/PM/APC strings are consumed and ignored
        IgnoreString,
    };

    void processChar(uint32_t ch);
    void transition(State newState);

    // UTF-8 decoding
    void feedByte(uint8_t byte);
    void flushCodepoint(uint32_t cp);

    ActionCallback m_callback;
    State m_state = Ground;

    // CSI accumulation
    std::vector<int> m_params;
    int m_currentParam = -1;  // -1 means "not started"
    std::string m_intermediate;
    std::string m_oscString;

    // UTF-8 decoder state
    uint32_t m_utf8Accum = 0;
    int m_utf8Remaining = 0;
    int m_utf8ExpectedLen = 0;
};
