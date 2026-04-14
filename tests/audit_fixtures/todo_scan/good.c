// Good-side canary: tokens that look like the rule's keywords but fail the
// post-token requirement of the regex (must be followed by `(`, `:`, or
// whitespace). Identifiers continuing into a word character are safe.

int TODO_count = 3;
const char *label = "TODOs";
int FIXMEs_handled = 0;
int HACKish_value = 1;
int XXXth_step = 2;

// Lower-case or unrelated English words — regex is case-sensitive
// todos on the list
// hackathon schedule
// fixme-less code

int main(void) { return TODO_count + FIXMEs_handled + HACKish_value + XXXth_step; }
