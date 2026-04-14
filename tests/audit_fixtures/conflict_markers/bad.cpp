// Unresolved merge conflict — each conflict marker should be flagged.
// Includes a diff3-style `|||||||` merge-base marker.
// @expect conflict_markers
// @expect conflict_markers
// @expect conflict_markers
// @expect conflict_markers
int answer() {
<<<<<<< HEAD
    return 42;
||||||| merged common ancestors
    return 40;
=======
    return 43;
>>>>>>> branch-b
}
