# Patches

Variants of tiny_c ship here as unified diffs. Apply with:

```sh
cd /path/to/tiny_c
patch -p1 < patches/<name>.diff
make
```

Patches are kept small and orthogonal. Each addresses one concern. No patch-on-patch fixups — rebase instead.

## Planned

| patch | adds | cost |
|---|---|---|
| `add-backtrace.diff` | SIGSEGV handler with `backtrace()` for crash debugging | +15 LOC |
| `add-bearssl.diff` | drops libcurl for a hand-rolled BearSSL HTTPS client — yields a ~300 KB hermetic static binary | +~200 LOC |
| `add-per-turn-rules.diff` | rules dynamically activate based on files the agent has touched this session (Claude Code parity) | +~50 LOC |
| `add-vision.diff` | image content block support for multimodal models | TBD |
| `add-mcp.diff` | Model Context Protocol client | TBD |

Contributions welcome. Keep them under 200 LOC each.
