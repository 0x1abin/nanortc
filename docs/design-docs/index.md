# Design Documents Index

## Authoritative Reference

| Document | Status | Description |
|----------|--------|-------------|
| [nanortc-design-draft.md](nanortc-design-draft.md) | **Active** | Full architecture design: Sans I/O model, protocol stack, crypto provider, platform integration, implementation plan. Single source of truth for all design decisions. |

## Verification Status

| Section | Verified Against | Status |
|---------|-----------------|--------|
| §1 Project Overview | — | Verified (Phase 0 skeleton matches) |
| §2 Architecture | Code structure | Verified (module layout matches) |
| §3 Protocol Stack | Code + RFCs | Updated: ICE supports controlled + controlling roles |
| §4 Crypto Provider | `crypto/nano_crypto.h` | Verified (mbedtls + OpenSSL dual backend) |
| §5 Platform Integration | `examples/` | Verified (Linux examples: datachannel, media_send) |
| §6 External Dependencies | `CMakeLists.txt` | Verified (mbedtls default, OpenSSL optional) |
| §7 Implementation Plan | `docs/exec-plans/` | Active (Phase 1 in progress) |
| §9 AI Development Guidelines | `AGENTS.md` | Verified (rules encoded) |

## How to Use

1. **Starting a new module?** Read the relevant section of `nanortc-design-draft.md` first.
2. **Protocol question?** Check §3 for the protocol stack overview, then consult the RFC directly.
3. **API question?** §2.2 has the complete public API specification.
4. **Design conflict?** The design doc is authoritative. If code disagrees with the doc, the doc wins (unless the doc is explicitly updated).
