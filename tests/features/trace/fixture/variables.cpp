// ANTS-6208: which variables are definitions. ANTS-6209: a call the parse cannot see.
namespace {
constexpr int traceConstant = 1;
const char traceArray[] = "x";
}
int traceGlobal;
struct TraceHolder {
    static constexpr int traceMember = 2;
};
#define TRACE_TRY(x) if (!(x)) return
int traceWrap(int);
int macroArgFn();
void useVariables() {
    int traceLocal = traceConstant;
    (void)traceLocal;
    TRACE_TRY(traceWrap(macroArgFn()))
}
