# Screenshots

UI captures referenced by `packaging/linux/org.ants.Terminal.metainfo.xml`
and rendered by GNOME Software / KDE Discover / Flathub's store tile.

## Current set

| File | Caption |
|------|---------|
| `01-main-terminal.png` | Main terminal — custom title bar, tab gradient, status bar |
| `02-review-changes.png` | Review Changes dialog — per-file diff + apply flow |
| `03-project-audit.png`  | Project Audit panel — clazy + cppcheck + grep findings |

## Replacement policy

- Keep filenames numeric-prefixed (`NN-name.png`) so the ordering
  survives sort; the metainfo block renders in file order.
- Width ≈ 1920px is the Flathub sweet spot (small enough for the
  store tile at ~620px, large enough for hi-DPI detail-view).
- Update the metainfo `<screenshots>` block in the same commit as
  any file rename — URLs are baked in literally.
