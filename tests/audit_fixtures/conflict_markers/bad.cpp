// Unresolved merge conflict — each conflict marker should be flagged.
// @expect conflict_markers
// @expect conflict_markers
// @expect conflict_markers
int answer() {
<<<<<<< HEAD
    return 42;
=======
    return 43;
>>>>>>> branch-b
}
