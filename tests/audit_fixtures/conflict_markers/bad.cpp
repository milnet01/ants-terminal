// Unresolved merge conflict — each conflict marker should be flagged.
// Includes a diff3-style `|||||||` merge-base marker.
// @expect markers are inline (one per flagged line), matching the
// convention used by every other audit_fixtures/<rule>/bad.* file.
int answer() {
<<<<<<< HEAD  // @expect conflict_markers
    return 42;
||||||| merged common ancestors  // @expect conflict_markers
    return 40;
=======  // @expect conflict_markers
    return 43;
>>>>>>> branch-b  // @expect conflict_markers
}
