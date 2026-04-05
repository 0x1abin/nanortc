# Architecture Constraints

Mechanical enforcement of NanoRTC design invariants. These constraints should be checked in CI.

## 1. Sans I/O Enforcement

No platform headers in `src/`:
```bash
# Must return empty
grep -rn '#include <sys/' src/
grep -rn '#include <pthread' src/
grep -rn '#include <time.h>' src/
grep -rn '#include <unistd' src/
grep -rn '#include <stdlib.h>' src/
```

## 2. No Dynamic Allocation

No malloc family calls in `src/`:
```bash
# Must return empty
grep -rn '\bmalloc\b\|calloc\b\|realloc\b\|\bfree\b' src/
```

Note: `memset`, `memcpy`, `memmove` from `<string.h>` are allowed.

## 3. Feature Flag Build Matrix

All six feature combinations must compile and pass tests:
```bash
# DATA:       DC=ON  AUDIO=OFF VIDEO=OFF
# AUDIO:      DC=ON  AUDIO=ON  VIDEO=OFF
# MEDIA:      DC=ON  AUDIO=ON  VIDEO=ON
# AUDIO_ONLY: DC=OFF AUDIO=ON  VIDEO=OFF
# MEDIA_ONLY: DC=OFF AUDIO=ON  VIDEO=ON
# CORE_ONLY:  DC=OFF AUDIO=OFF VIDEO=OFF
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=OFF -DNANORTC_FEATURE_VIDEO=OFF
cmake --build build && ctest --test-dir build --output-on-failure
```

## 4. Code Formatting

```bash
clang-format --dry-run --Werror src/*.c src/*.h include/*.h crypto/*.h crypto/*.c
```

## 5. Public Symbol Naming

All exported symbols must use allowed module prefixes (`nano_`, `nanortc_`, `stun_`, `ice_`, `dtls_`, `nsctp_`, `sctp_`, `dc_`, `sdp_`, `rtp_`, `rtcp_`, `srtp_`, `jitter_`, `bwe_`, `h264_`, `media_`, `ssrc_map_`, `addr_`, `track_`):
```bash
ALLOWED='nano_|nanortc_|stun_|ice_|dtls_|nsctp_|sctp_|dc_|sdp_|rtp_|rtcp_|srtp_|jitter_|bwe_|h264_|media_|ssrc_map_|addr_|track_'
nm -g libnanortc.a | grep ' T ' | awk '{print $3}' | grep -v '^_' | grep -vE "^($ALLOWED)"
# Must return empty
```

## 6. No Global Mutable State

```bash
# Must return empty (no non-const globals in src/)
nm libnanortc.a | grep ' [BD] ' | grep -v '__' | grep -v 'crc32c_table'
# Only const tables (like CRC lookup) are acceptable
```

## 7. No Unbounded String Functions

No `strlen`, `sprintf`, `snprintf`, `strcpy`, `strncpy`, `strcat`, `strncat`, `sscanf`, `atoi`, `atol`, or `gets` in `src/` or `crypto/`. API boundary uses annotated with `NANORTC_SAFE` are exempt. See [safe-c-guidelines.md](safe-c-guidelines.md).

```bash
# Must return empty (excluding NANORTC_SAFE-annotated API boundary lines)
grep -rnE '\b(strlen|sprintf|snprintf|strcpy|strncpy|strcat|strncat|sscanf|atoi|atol|gets)\b' src/ crypto/ \
  | grep -v 'NANORTC_SAFE'
```

## 8. No Hardcoded Array Sizes

Struct array members in headers must use named macros, not bare integer literals:

```bash
# Must return empty (excluding comments)
grep -rnE '\b(uint8_t|char|int8_t|uint16_t|uint32_t)\s+\w+\[\s*[0-9]+\s*\];' src/*.h include/nanortc.h \
  | grep -v '//'
```
