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

## 3. Profile Build Matrix

All three profiles must compile and pass tests:
```bash
for profile in DATA AUDIO MEDIA; do
    cmake -B build-${profile} -DNANORTC_PROFILE=${profile}
    cmake --build build-${profile}
    ctest --test-dir build-${profile} --output-on-failure
done
```

## 4. Code Formatting

```bash
clang-format --dry-run --Werror src/*.c src/*.h include/*.h crypto/*.h crypto/*.c
```

## 5. Public Symbol Naming

All exported symbols must start with `nano_`:
```bash
nm -g libnanortc.a | grep ' T ' | awk '{print $3}' | grep -v '^nano_' | grep -v '^_'
# Must return empty (except compiler-generated symbols)
```

## 6. No Global Mutable State

```bash
# Must return empty (no non-const globals in src/)
nm libnanortc.a | grep ' [BD] ' | grep -v '__' | grep -v 'crc32c_table'
# Only const tables (like CRC lookup) are acceptable
```

## 7. No Unbounded String Functions

No `strlen`, `sprintf`, `snprintf`, `strcpy`, `strncpy`, `strcat`, `strncat`, `sscanf`, `atoi`, `atol`, or `gets` in `src/` or `crypto/`. API boundary uses annotated with `NANO_SAFE` are exempt. See [safe-c-guidelines.md](safe-c-guidelines.md).

```bash
# Must return empty (excluding NANO_SAFE-annotated API boundary lines)
grep -rnE '\b(strlen|sprintf|snprintf|strcpy|strncpy|strcat|strncat|sscanf|atoi|atol|gets)\b' src/ crypto/ \
  | grep -v 'NANO_SAFE'
```

## 8. No Hardcoded Array Sizes

Struct array members in headers must use named macros, not bare integer literals:

```bash
# Must return empty (excluding comments)
grep -rnE '\b(uint8_t|char|int8_t|uint16_t|uint32_t)\s+\w+\[\s*[0-9]+\s*\];' src/*.h include/nanortc.h \
  | grep -v '//'
```
