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
- Use `nanortc_htons` / `nanortc_htonl` for encoding; never assume host byte order
- State structs: group related fields, keep frequently-accessed fields near top

## Naming

- Public symbols: `nano_` prefix (e.g., `nanortc_init`)
- Internal functions: module prefix (e.g., `stun_parse`, `sctp_handle_data`)
- Static (file-local) helpers: no prefix required
- Types: `nano_*_t` (public), `*_t` (internal)
- Enums: `NANORTC_*` (public), `*_STATE_*` / `*_TYPE_*` (internal)
- Macros: `NANORTC_*` (public), `STUN_*` / `SCTP_*` etc. (internal, module-prefixed)
- **No ad-hoc abbreviations.** Prefer full names over self-invented abbreviations that other developers may not recognize. For example, use `libdatachannel` not `libdc`, use `datachannel` not `dc` (except in established protocol terms like DCEP). Well-known abbreviations from RFCs and standards (STUN, SCTP, DTLS, SDP, SRTP, ICE, RTP, RTCP, BWE, DCEP) are acceptable. When in doubt, spell it out.

## Error Handling

```c
int result = some_function(args);
if (result != NANORTC_OK) {
    return result;  // propagate error
}
```

- Never use `assert()` in `src/` — return `NANORTC_ERR_*` codes
- `assert()` is allowed in `tests/` for test assertions
- Check all pointer parameters for NULL at public API boundaries

## Comments

- `/* */` for documentation and multi-line comments
- `//` for short inline comments
- RFC references: `/* RFC 8489 Section 6.1 */`

## Public API Documentation (Doxygen)

All public headers in `include/` use Doxygen `/** */` format. Internal code in `src/` keeps plain `/* */`.

**Functions:**
```c
/**
 * @brief Initialize the RTC state machine.
 *
 * @param rtc   Caller-allocated state (must be zeroed).
 * @param cfg   Configuration (pointer must stay valid during init).
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_INVALID_PARAM  rtc or cfg is NULL.
 */
NANORTC_API int nanortc_init(nanortc_t *rtc, const nanortc_config_t *cfg);
```

**Types / structs:**
```c
/** @brief Network-agnostic socket address (IPv4 / IPv6). */
typedef struct nanortc_addr { ... } nanortc_addr_t;
```

**Struct fields / enum values** (trailing comment):
```c
typedef enum {
    NANORTC_LOG_ERROR = 0, /**< Unrecoverable errors. */
    NANORTC_LOG_WARN  = 1, /**< Unusual but recoverable. */
} nanortc_log_level_t;
```

**Macros (config):**
```c
/** @brief Maximum number of DataChannels. */
#ifndef NANORTC_MAX_DATACHANNELS
#define NANORTC_MAX_DATACHANNELS 8
#endif
```

**File-level `@file`** is optional — not required.

## Array Size Naming

Every struct array member must use a named macro for its size — never a bare integer literal.

| Category | Defined in | Example |
|----------|-----------|---------|
| Configurable buffer | `nanortc_config.h` (`#ifndef` guard) | `NANORTC_ICE_UFRAG_SIZE`, `NANORTC_STUN_BUF_SIZE` |
| Protocol-fixed constant | Module header (`#define`) | `STUN_TXID_SIZE`, `NANORTC_SRTP_KEY_SIZE` |

```c
/* Good */
char local_ufrag[NANORTC_ICE_UFRAG_SIZE];
uint8_t transaction_id[STUN_TXID_SIZE];

/* Bad */
char local_ufrag[8];
uint8_t transaction_id[12];
```

Boundary checks in `.c` files must reference the same macro:

```c
/* Good */
if (addr_len >= NANORTC_IPV6_STR_SIZE) { return NANORTC_ERR_PARSE; }

/* Bad */
if (addr_len > 45) { return NANORTC_ERR_PARSE; }
```

## Return Value Convention

All `nano_*` public API functions return `int` as a status code:

- `NANORTC_OK` (0) = success
- `NANORTC_ERR_*` (negative) = failure

**Never return positive values.** Output lengths are passed via `size_t *out_len` parameters. Use `nanortc_err_to_name()` to convert error codes to human-readable strings for diagnostics.

```c
/* Good: status code + out_len */
size_t answer_len = 0;
int rc = nanortc_accept_offer(&rtc, offer, answer, sizeof(answer), &answer_len);
if (rc != NANORTC_OK) {
    fprintf(stderr, "failed: %s\n", nanortc_err_to_name(rc));
    return rc;
}

/* Bad: using return value as length */
int len = nanortc_accept_offer(&rtc, offer, answer, sizeof(answer), NULL);
if (len > 0) { ... }  // WRONG — rc is always 0 or negative
```

## Code Style

Enforced by `.clang-format`:
- 4-space indent, no tabs
- 100-column limit
- Linux (K&R) brace style
- `char *ptr` (pointer right-aligned)
