# Coding Standards

## Include Order

1. Own header (e.g., `nano_stun.c` includes `"nano_stun.h"` first)
2. Internal NanoRTC headers (`"nano_*.h"`)
3. C standard headers (`<string.h>`, `<stdint.h>`, etc.)

Never include platform or OS headers in `src/`.

## Buffer Passing Conventions

Input buffer:
```c
int foo_parse(const uint8_t *data, size_t len, foo_t *out);
```

Output buffer:
```c
int foo_encode(const foo_t *in, uint8_t *buf, size_t buf_len, size_t *out_len);
```

Always pass `(buffer, buffer_length)` as a pair. Output functions take an additional `size_t *out_len` to report actual bytes written.

## Struct Layout

- Wire-format structs: use fixed-size fields (`uint8_t`, `uint16_t`, `uint32_t`)
- Use `nano_htons` / `nano_htonl` for encoding; never assume host byte order
- State structs: group related fields, keep frequently-accessed fields near top

## Naming

- Public symbols: `nano_` prefix (e.g., `nano_rtc_init`)
- Internal functions: module prefix (e.g., `stun_parse`, `sctp_handle_data`)
- Static (file-local) helpers: no prefix required
- Types: `nano_*_t` (public), `*_t` (internal)
- Enums: `NANO_*` (public), `*_STATE_*` / `*_TYPE_*` (internal)
- Macros: `NANO_*` (public), `STUN_*` / `SCTP_*` etc. (internal, module-prefixed)

## Error Handling

```c
int result = some_function(args);
if (result != NANO_OK) {
    return result;  // propagate error
}
```

- Never use `assert()` in `src/` — return `NANO_ERR_*` codes
- `assert()` is allowed in `tests/` for test assertions
- Check all pointer parameters for NULL at public API boundaries

## Comments

- `/* */` for documentation and multi-line comments
- `//` for short inline comments
- RFC references: `/* RFC 8489 Section 6.1 */`

## Code Style

Enforced by `.clang-format`:
- 4-space indent, no tabs
- 100-column limit
- Linux (K&R) brace style
- `char *ptr` (pointer right-aligned)
