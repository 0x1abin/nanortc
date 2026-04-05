# Safe C Guidelines — NanoRTC

Safe memory operation rules for `src/` and `crypto/` directories.
Target: RTOS/embedded systems where memory safety is a hard constraint.

## Core Principles

1. **Explicit length propagation** — every buffer carries a `_len` field.
2. **Parse at boundary** — NUL-terminated strings convert to `(ptr, len)` once at API entry; internal code never scans for NUL.
3. **No implicit scanning** — no function that walks memory for a sentinel value.

## Banned Functions

| Function   | Risk | Safe Alternative |
|------------|------|------------------|
| `strlen`   | Unbounded NUL scan | `_len` field or `nanortc_strnlen(s, maxlen)` |
| `sprintf`  | Unbounded write | Manual formatting with known buffer sizes |
| `snprintf` | NUL-scans `%s` args; libc printf code bloat; hard to audit | Manual formatting (hex table lookup, `memcpy` at offset) |
| `strcpy`   | Unbounded copy | `memcpy` + explicit length |
| `strncpy`  | NUL padding, no termination guarantee | `memcpy` + explicit NUL |
| `strcat` / `strncat` | Scans dest for NUL | `memcpy` at known offset |
| `sscanf`   | Complex implicit scanning | Manual parsing |
| `atoi` / `atol` | No error detection, UB on overflow | `nanortc_parse_uint32(buf, len, &out)` |
| `gets`     | Unbounded read (removed in C11) | Never use |

## Allowed Functions

`memcpy`, `memmove`, `memset`, `memcmp` — all require explicit length.

Caller invariants:
- `len ≤ dst_buffer_size` (and `≤ src_buffer_size` for copy)
- Use `memmove` if `src`/`dst` may overlap

## Safe Patterns (Quick Reference)

```c
/* Compile-time literal length */
#define PREFIX     "candidate:"
#define PREFIX_LEN (sizeof(PREFIX) - 1)

/* Hex formatting — replaces snprintf(buf, 4, "%02X:", val) */
static const char hex_upper[] = "0123456789ABCDEF";
buf[0] = hex_upper[(val >> 4) & 0xF];
buf[1] = hex_upper[val & 0xF];
buf[2] = ':';

/* Overflow-safe concatenation — replaces snprintf(buf, sz, "%s%s", a, b) */
if (a_len > buf_size || b_len > buf_size - a_len) return NANORTC_ERR_OVERFLOW;
memcpy(buf, a, a_len);
memcpy(buf + a_len, b, b_len);

/* String copy to fixed buffer — replaces strcpy */
if (src_len > sizeof(ctx->field)) return NANORTC_ERR_OVERFLOW;
memcpy(ctx->field, src, src_len);
ctx->field_len = src_len;

/* Integer parsing — replaces atoi / sscanf */
uint32_t port;
if (nanortc_parse_uint32(port_str, port_str_len, &port) != 0 || port > 65535)
    return NANORTC_ERR_INVALID;
```

## Crypto-Specific: Sensitive Data Clearing

Bare `memset(..., 0, ...)` on key material may be optimized away. Use:

```c
static inline void nanortc_memzero(void *buf, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)buf;
    for (size_t i = 0; i < len; i++) p[i] = 0;
}
```

Or platform-provided `explicit_bzero` / `memset_s` if available.

## API Boundary Exception

Public `nano_*` functions may use `strlen` **once per string parameter** at entry, annotated:

```c
size_t sdp_len = strlen(sdp); /* NANORTC_SAFE: API boundary */
```

Functions with >3 string parameters should provide a `_n` variant with explicit lengths.
Internal functions **never** call `strlen`.

## CI Enforcement

```bash
BANNED='strlen|sprintf|snprintf|strcpy|strncpy|strcat|strncat|sscanf|atoi|atol|gets'
grep -rnE "\b($BANNED)\b" src/ crypto/ \
  | grep -v 'NANORTC_SAFE' \
  | grep -v '^\s*//' \
  | grep -v '^\s*\*' \
  || { echo "❌ Banned function found"; exit 1; }
```

Limitation: macro wrappers (`#define COPY strcpy`) bypass grep — agent audit must catch these.

## Agent Review Checklist

| # | Check | Flag if |
|---|-------|---------|
| 1 | **Banned functions** | Any banned function outside `NANORTC_SAFE` lines; macro aliases for banned functions |
| 2 | **Length propagation** | `char[]`/`uint8_t[]` buffer without `_len` field; `_len` not set on write or not read on consumption |
| 3 | **memcpy bounds proof** | No visible proof that `len ≤ dest_size` (sizeof, bounds check, or documented invariant) |
| 4 | **API boundary** | `strlen` without `NANORTC_SAFE` annotation; result re-scanned later; >3 string params without `_n` variant |
| 5 | **Crypto zeroing** | Bare `memset(...,0,...)` on key/HMAC/cipher material — must be `nanortc_memzero`; missing zeroing on error paths |
| 6 | **Integer parsing** | `atoi`/`strtol` without full error+overflow check; unbounded string-to-int conversion |
| 7 | **Length arithmetic overflow** | `a_len + b_len` used without prior overflow check (especially on 16-bit `size_t` targets) |