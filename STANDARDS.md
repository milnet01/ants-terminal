# Development Standards

## Code Style

- **PEP 8** for all Python code
- **Type hints** on function signatures (parameters and return types)
- Use `dataclass` for data structures
- Use f-strings for string formatting
- Imports grouped: stdlib, third-party, local — separated by blank lines

## Naming

- `snake_case` for functions, methods, variables, and module files
- `PascalCase` for classes
- `UPPER_SNAKE_CASE` for module-level constants
- Private methods/attributes prefixed with `_`
- Signal names should describe the event: `submitted`, `image_pasted`, `enter_pressed`

## Architecture

- **Signals/slots** for all cross-widget communication — no direct method calls between siblings
- **Config class** for all persistent state — never write JSON directly
- **Theme dict** as the single source of truth for colors — no hardcoded color values in widgets
- **Message dataclass** as the canonical data format for chat entries
- Each module should have a single clear responsibility

## Widget Guidelines

- Widgets emit signals; parent widgets connect them
- Widgets do not import or reference their parent
- Keep widget constructors simple — use `_setup_ui()` for complex layouts
- Use `setObjectName()` for widgets that need specific QSS styling

## Error Handling

- Graceful fallbacks for missing config files
- Never crash on image paste failure — log and ignore
- Config load failures fall back to defaults silently

## Testing

- Manual testing for UI interactions
- Each new widget should be independently instantiable for testing
- Theme changes should be visually verified across all themes

## Git

- Commit messages: imperative mood, concise subject line
- One logical change per commit
- Branch naming: `feature/<name>`, `fix/<name>`, `refactor/<name>`
