# Core Beliefs

Opinionated principles that define how NanoRTC is built. These are non-negotiable constraints, not suggestions. They apply to every line of code, whether written by human or agent.

## 1. Sans I/O is the foundation, not a feature

NanoRTC is a pure state machine. This is not an optimization or a nice-to-have — it is the architectural identity of the project. Every design decision flows from this constraint.

**Implication:** If you need to choose between a simpler implementation that touches OS APIs and a harder one that stays pure, always choose the harder one. The moment `src/` includes a platform header, the entire value proposition breaks.

## 2. RFC is the authority, code is the implementation

When implementing a protocol module, the relevant RFC is the specification. Reference implementations (str0m, libdatachannel) are useful for understanding, but if they disagree with the RFC, the RFC wins.

**Implication:** Always cite the RFC section number in code comments when implementing wire formats or state machines. This makes the code verifiable.

## 3. Parse at the boundary, trust internally

All external data (UDP packets, SDP strings) is validated at the entry point. Once parsed into internal structs, code trusts the validated data without re-checking.

**Implication:** Parser functions must handle all malformed input gracefully (return error codes, never crash). Internal functions can assume their inputs are valid.

## 4. Compile-time, not runtime

Feature selection happens at compile time via orthogonal feature flags (`NANO_FEATURE_DATACHANNEL`, `NANO_FEATURE_AUDIO`, `NANO_FEATURE_VIDEO`), not at runtime. There is no dead code in a built binary — if video is not enabled, no video code is compiled.

**Implication:** Use `#if NANO_FEATURE_*` guards, not runtime `if` checks. This keeps the binary small and deterministic for embedded targets.

## 5. Caller owns resources

NanoRTC never allocates memory, creates threads, or opens sockets. The caller provides all resources (buffers, time, network I/O). This makes the library composable with any RTOS and any memory strategy.

**Implication:** Function signatures always take caller-provided buffers with explicit size parameters. Return values indicate how much of the buffer was used.

## 6. One dependency, fully abstracted

The only external dependency is mbedtls, and even that is behind the `nano_crypto_provider_t` interface. The library can be ported to any crypto backend by implementing a single struct of function pointers.

**Implication:** Never call mbedtls functions directly from `src/`. Always go through `crypto/nano_crypto.h`.

## 7. Mechanical enforcement over documentation

Rules that aren't enforced mechanically will be violated. Every constraint in this document has a corresponding CI check, linter, or build-system rule.

**Implication:** When adding a new constraint, also add the enforcement mechanism. A rule without a check is just a wish.

## 8. Agent legibility is a design goal

The codebase is optimized for AI agent comprehension. This means: consistent naming, explicit module boundaries, self-documenting structure, and all context living in the repository (not in people's heads or chat threads).

**Implication:** If a design decision isn't obvious from reading the code and docs, write it down. If context exists only in a conversation, it needs to be captured in `docs/`.

## 9. Small, correct, complete — in that order

For embedded targets, code size matters. But correctness always comes before optimization, and completeness (handling all RFC-required cases) comes before adding features.

**Implication:** Don't skip error paths to save code size. Don't add optional features before the core protocol is fully correct.
