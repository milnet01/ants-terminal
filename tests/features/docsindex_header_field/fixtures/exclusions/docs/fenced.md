# Exclusion fixture — a fenced block inside the header block

**Status:** draft
~~~

~~~
absorbed
**Kind:** doc.

## 1. Body

scanDoc skips fenced lines with a `continue`, so the surviving lines close
up and `absorbed` becomes adjacent to the Status field. headerField over the
RAW lines instead treats the fence opener as a continuation and stops at the
blank line inside the fence. The two therefore disagree by design — which is
the bound INV-2 names, asserted rather than left implicit.
