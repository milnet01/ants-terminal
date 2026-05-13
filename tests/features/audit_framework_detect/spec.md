# audit_framework_detect — AuditHygiene::detectProjectFrameworks +
# semgrepRulePacks (ANTS-1111)

## INVs

- INV-4: detectProjectFrameworks returns:
  - ["flask"] for a project with requirements.txt mentioning `flask`
  - ["react"] for a project with package.json `"react": "..."` dep
  - ["qt6"] for a project with `find_package(Qt6` in CMakeLists.txt
  - [] for an empty project
- semgrepRulePacks emits two args per recognised framework
  (`{"--config", "p/<fw>"}`); unknown framework names contribute none.
