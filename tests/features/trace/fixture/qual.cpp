// ANTS-6204: a scope narrows callers and references.
namespace detail { int qualNamed() { return 1; } }
namespace other { int qualNamed() { return 2; } }
void qualCalls(Thing &obj) {
    detail::qualNamed();
    other::qualNamed();
    obj.qualNamed();
}
