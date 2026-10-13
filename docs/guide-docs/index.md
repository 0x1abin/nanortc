# Guide Documents Index

Operator-facing how-to guides: commands, flags, workflow recipes. Distinct from `engineering/` (standards, mechanical constraints) and `design-docs/` (architecture and reasoning).

## Available Guides

| Document | Description |
|----------|-------------|
| [0.2 migration](../engineering/nanortc-02.md) | Breaking API, output ownership, authentication, lifecycle and acceptance |
| [build.md](build.md) | Build commands, feature flags, crypto backends, interop, ASan, fuzz, coverage, ESP-IDF, formatting, CI locally |

## Naming Conventions

- kebab-case topic or verb — `build.md`, `testing.md`, `fuzzing.md`, `release.md`
- No redundant `-guide` suffix (the directory is `guide-docs/`)
- If a guide grows past ~250 lines, split it rather than letting it bloat
