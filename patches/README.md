# Patches

Variants of sa ship here as unified diffs. Apply with:

```sh
cd /path/to/sa
patch -p1 < patches/<name>.diff
make
```

Patches are kept small and orthogonal. Each addresses one concern. No patch-on-patch fixups — rebase instead.

## Available

| patch | adds | cost |
|---|---|---|
| `add-confirm-mode.diff` | prompt `y/n/always` on every `shell` / `edit_file` call; `SA_NO_CONFIRM=1` bypass; reads from `/dev/tty` so piped stdin still works | +~30 LOC |
| `add-thinking.diff` | `THINKING=low\|med\|high` env var enables Kimi/Anthropic extended thinking (1024/4096/16000 budget tokens). Streams `thinking_delta` dimmed to stderr; drops thinking blocks from the assistant `content` array but echoes the concatenated reasoning back as `reasoning_content` on the assistant message so Kimi accepts multi-turn tool use. No Anthropic-style signature round-trip — works on Kimi, may need tweaks for pure Anthropic. | +31 LOC |

## Planned

| patch | adds | cost |
|---|---|---|
| `add-backtrace.diff` | SIGSEGV handler with `backtrace()` for crash debugging | +15 LOC |
| `add-per-turn-rules.diff` | rules dynamically activate based on files the agent has touched this session (Claude Code parity) | +~50 LOC |
| `add-vision.diff` | image content block support for multimodal models | TBD |
| `add-mcp.diff` | Model Context Protocol client | TBD |

Contributions welcome. Keep them under 200 LOC each.
