/* Fixture: a header carrying no QT_VERSION_STR at all. The guard must
   REFUSE on this, never pass — a guard that cannot read its input and
   reports OK is indistinguishable from one that checked and found
   nothing wrong. */
#define QT_FEATURE_something 1
