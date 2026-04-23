# sa

A suckless-style streaming agent in C. One binary, one config file, no runtime.

~1000 lines of portable C, zero link-time deps beyond libc (BearSSL vendored, statically linked). Talks to any Anthropic-compatible `/v1/messages` endpoint (Kimi, Anthropic direct). No JS runtime, no React reconciler, no framework.

See `ROADMAP.md` for the thesis and philosophy.

## Build

No runtime deps beyond libc. BearSSL is vendored at `bearssl/` and statically linked.

```sh
# any Unix with a C compiler and make
make && sudo make install
```

First build auto-copies `config.def.h` → `config.h`. Edit `config.h` to customise, then `make`.

Install prefix defaults to `/usr/local`; override with `PREFIX=$HOME/.local make install`.

## Configure

Compile-time in `config.h`: `BASE_SYSTEM`, buffer caps, tool iteration limit, session filename.

Runtime via environment:

| var | default | purpose |
|---|---|---|
| `KIMI_TOKEN` | — (required) | API key |
| `KIMI_BASE_URL` | `https://api.kimi.com/coding` | endpoint base |
| `MODEL` | `kimi-for-coding` | model id sent in request |
| `TINY_DEBUG` | unset | dump raw SSE chunks to stderr |

## Use

```sh
sa "find all TODO comments in this repo"   # one-shot
sa                                          # REPL
sa -c                                       # resume REPL from .sa_session.jsonl
sa -c "continue"                            # resume + one-shot
```

Ctrl+C interrupts the current turn (returns to REPL prompt). Ctrl+D exits. `rm .sa_session.jsonl` to truly clear history.

## Context loading

Auto-loaded into the system prompt on startup, in order:

1. `BASE_SYSTEM` from `config.h`
2. `~/.claude/CLAUDE.md`, then `./CLAUDE.md`
3. Rules from `~/.claude/rules/*.md` and `./.claude/rules/*.md`
   - No frontmatter → unconditional
   - `paths: [glob]` frontmatter → only if `find -name <glob>` finds a match in cwd tree
4. Skills from `~/.claude/skills/*.md` and `./.claude/skills/*.md` (plus `<dir>/SKILL.md` form)
   - Injected as `<skills>` XML block; model reads SKILL.md on demand

## Tools

| tool | purpose |
|---|---|
| `shell` | `/bin/sh -c <cmd>`, combined stdout+stderr, exit/signal code, 1 MB output cap |
| `read_file` | line-numbered, `offset`/`limit` paging, 256 KB byte cap, NUL-byte detection |
| `edit_file` | empty `old_string` → write full contents; non-empty → unique-match replace. Atomic via `.tmp` + rename. 4 MB file cap. |

`write_file` merged into `edit_file`. `list_dir` omitted — use `ls` via `shell`.

## Patches

Variants live in `patches/`. Not a plugin system — apply with `patch -p1 < patches/<name>.diff`, rebuild.

## License

TBD.
