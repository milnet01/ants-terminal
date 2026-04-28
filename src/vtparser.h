#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

// A single action the parser emits for the grid to execute
struct VtAction {
    enum Type {
        Print,          // Regular character to display. Either single `codepoint`
                        // (UTF-8 decoded, may be wide / combining) or a run of
                        // printable-ASCII bytes pointed to by `printRun`.
        Execute,        // C0 control char (BEL, BS, HT, LF, CR, etc.)
        CsiDispatch,    // CSI sequence complete (cursor move, erase, SGR, etc.)
        EscDispatch,    // ESC sequence complete
        OscEnd,         // OSC sequence complete (window title, etc.)
        DcsEnd,         // DCS sequence complete (Sixel, etc.)
        ApcEnd,         // APC sequence complete (Kitty graphics, etc.)
    };

    Type type;
    uint32_t codepoint = 0;       // For Print (scalar path: single decoded codepoint)
    // For Print coalesced runs: pointer into the caller's feed() buffer +
    // length. Only valid for the duration of the ActionCallback invocation;
    // consumers must copy bytes out before returning if they need them later.
    // When `printRun != nullptr`, `codepoint` is zero and the consumer must
    // treat this as a run of printable-ASCII bytes (each in [0x20..0x7E]).
    //
    // LIFETIME WARNING: consumers that store VtActions past the callback
    // (e.g. VtStream::m_pending, which outlives onPtyData) MUST expand the
    // run into per-byte Print actions — the feed-buffer memory is gone by
    // the time the stored action is dispatched. The direct-callback path
    // (bench, tests, synchronous TerminalGrid::processAction) is safe
    // because the action is consumed on the stack before the callback
    // returns.
    const char *printRun = nullptr;
    int printRunLen = 0;
    char controlChar = 0;         // For Execute
    char finalChar = 0;           // For CSI/ESC dispatch
    std::vector<int> params;      // CSI parameters
    std::vector<bool> colonSep;   // true if param[i] was preceded by ':' (sub-parameter)
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
        OscStringEsc,   // peek-state: just saw ESC inside OSC body — see vtparser.cpp
        DcsString,      // DCS sequence (Sixel graphics)
        DcsStringEsc,   // peek-state: just saw ESC inside DCS body
        ApcString,      // APC sequence (Kitty graphics)
        ApcStringEsc,   // peek-state: just saw ESC inside APC body
        IgnoreString,   // SOS, PM — consumed and ignored
        IgnoreStringEsc,// peek-state: just saw ESC inside SOS/PM body
    };

    void processChar(uint32_t ch);
    void transition(State newState);

    // UTF-8 decoding
    void feedByte(uint8_t byte);
    void flushCodepoint(uint32_t cp);

    // Append a codepoint to an OSC/DCS/APC accumulator as UTF-8, bounded by maxBytes
    static void appendUtf8(std::string &out, uint32_t ch, size_t maxBytes);

    ActionCallback m_callback;
    State m_state = Ground;

    // CSI accumulation
    std::vector<int> m_params;
    std::vector<bool> m_colonSep;  // parallel to m_params: true if preceded by ':'
    bool m_nextIsSubParam = false; // set by ':' separator for next param
    int m_currentParam = -1;  // -1 means "not started"
    std::string m_intermediate;
    std::string m_oscString;
    std::string m_dcsString;  // DCS payload (Sixel data)
    std::string m_apcString;  // APC payload (Kitty graphics)

    // UTF-8 decoder state
    uint32_t m_utf8Accum = 0;
    int m_utf8Remaining = 0;
    int m_utf8ExpectedLen = 0;
};
