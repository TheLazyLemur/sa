# tiny_c — roadmap

Minimal suckless-style agent in C. One binary, one config file, no runtime.

Name: `sa` (simple/suckless agent).

## Thesis

Agents don't need a JS runtime, a React reconciler, or 4 GB of RAM. A correct streaming agent with tool dispatch is ~1000 lines of C. This project proves it and stays there.

Reference category: `rg`, `fd`, `dmenu`, `jq`. Not `claude-code`, not `aider`, not a framework.

## Targets

- LOC budget: < 1200 lines (`wc -l` on `main.c`, suckless convention — blanks and comments included)
- Binary size: < 200 KB (static link with vendored BearSSL, macOS or Linux)
- Cold start: < 20 ms
- Resident memory: < 10 MB
- Runs on: Pi Zero 2 W, any machine with libcurl
- Single `main.c` + `config.h` + `Makefile`. No build system, no package manager.

## Philosophy

- **Compile-time configuration**: edit `config.h`, recompile. No runtime flags beyond env vars.
- **No plugins**: tools are static functions in the binary. Audit once at compile time.
- **Patches over features**: upstream ships the minimum. Variants live as `.patch` files.
- **Unconditional behaviour**: no matchers, no globs, no conditionals. Delete the file to disable.
- **Unix citizenship**: stdin/stdout/stderr, SIGINT, exit codes, pipes.

## Milestones

### 0.1 — Kernel — done

- `POST /v1/messages` with SSE streaming
- Parse both `data: {...}` and `data:{...}` (Anthropic + Kimi)
- Content-block accumulator (text, tool_use, thinking)
- `shell` tool with combined stdout+stderr and exit code
- SIGINT aborts stream cleanly (`CURLE_WRITE_ERROR` path)
- Env vars: `KIMI_TOKEN`, `KIMI_BASE_URL`, `MODEL`, `SA_DEBUG`
- Session continuation: `.sa_session.jsonl` in cwd, `-c` / `--continue`

### 0.2 — Core tools — done

- `read_file` — bounded (line offset/limit, 256 KB byte cap, line-numbered via `%6ld\t`); NUL-byte detection; `getline()` for arbitrary line lengths; suggests shell for binary
- `edit_file` — unified mutate tool: empty `old_string` writes `new_string` as full file contents (create/overwrite), non-empty requires unique match and replaces in place. Atomic via `.tmp` + rename. 4 MB file cap.
- Tool schemas built via `add_prop` / `add_required` / `make_tool` helpers rather than a `config.h` static table (simpler with the current tool count)

`write_file` merged into `edit_file` — one mutate tool beats two overlapping ones (matches `sed` mental model). `list_dir` deliberately omitted — shell's `ls` covers every shape (flat, with metadata, filtered, recursive).

### 0.3 — Context loading — done

- `CLAUDE.md` injection from CWD + `~/.claude/CLAUDE.md`
- `.claude/rules/*.md` scanned from CWD (per-project) and `~/.claude/rules/` (global)
- Minimal YAML frontmatter support: `paths: [glob]` list — rule loads only if `find -name <glob>` matches a file in cwd tree
- No frontmatter → rule loads unconditionally (matches `tdd.md`, `testing_rfc.md` style)
- `**/` prefix stripped before glob match so `**/*.cs` becomes `*.cs`
- Per-session evaluation only (scanned once at startup, not rebuilt per turn)
- Skip files > 64 KB (sanity)

`AGENTS.md` deliberately omitted — `CLAUDE.md` already covers the "per-project instructions" role. Per-turn rule activation (Claude Code's dynamic model where rules join/leave based on currently-touched files) rejected — +50 LOC for marginal gain over per-session.

### 0.4 — Skills — done

- Scan `.claude/skills/*.md` (flat) and `.claude/skills/<name>/SKILL.md` (directory form) from CWD and `~/.claude/skills/`
- Parse YAML frontmatter: `name:`, `description:` (supports `>` folded and `|` literal block scalars for multi-line values)
- `<skills>` XML block injected into system prompt with `{name, description, path}` entries
- System-prompt hint tells the model to `cat <path>` to load a skill's SKILL.md on demand (no dedicated `skill` tool — `read_file` + shell already do this)
- No auto-triggering, no glob activation, no chained loading
- 128 skill cap with log-once warning when reached

### 0.5 — Hardening — done (except SIGSEGV backtrace)

Real memory-safety work, not cosmetic. Each item closed a specific gap found by audit.

Done:
- Bound `block_t.text` / `partial` growth — 1 MB cap per buf, log-once on truncation (defeats streamed-text DoS)
- Bound `st->line` — 1 MB cap, drops remainder to next `\n`, logs (defeats no-newline DoS)
- `MAX_BLOCKS` — `block_overflow_warn()` logs once when exceeded
- NUL-byte safety in `read_file` via `memchr` per `getline` line
- `g_skills[128]` — log-once when cap hit
- `session_load` — logs malformed JSONL lines instead of silently skipping
- Path `snprintf` truncation — checked at 4 sites (home CLAUDE.md, home skills, scan path, SKILL.md)
- Anthropic error envelope — `raw_head[4096]` captures first 4 KB; on non-200, parses `error.message` to stderr
- `curl` timeouts: 10 s connect, 30 s low-speed abort
- `run_shell` 1 MB output cap now appends `[output truncated at 1MB]` marker
- `malloc` / `realloc` NULL → `abort()`
- `sigaction` (no `SA_RESTART`) so Ctrl+C interrupts `fgets` in the REPL
- `curl_slist_append` NULL chain + cleanup on alloc failure
- `cJSON_IsNumber` check before `valueint`
- `blocks_to_content` skips empty-type slots (don't emit `{"type":""}`)
- `realpath(src, NULL)` — avoid PATH_MAX overrun on Linux (4096 into 1024 buffer)
- `WIFEXITED` / `WIFSIGNALED` before `WEXITSTATUS`
- Mid-stream `error` SSE event surfaced to stderr

Pending (deferred to patch):
- SIGSEGV handler with `backtrace()` for debug builds — `backtrace_symbols()` isn't async-signal-safe, `lldb` + core file is strictly better for the rare crash that makes it past the above. Ship as `patches/add-backtrace.diff` if needed.

### 0.6 — Compaction — done

Reactive auto-compaction via tool-result clearing only. No summarisation LLM call.

- Trigger: API 400 with `"context"`, `"token"`, or `"length"` in the error message (returned as `-3` from `do_turn`)
- Clearing: walk messages before the last 10, find `tool_result` blocks, replace `content` string with `"[cleared]"`. `tool_use` records preserved so the agent knows it *did* call `read_file(foo.c)` — it can re-read if it still needs the content.
- Pair-safe: `tool_use`/`tool_result` pairing stays intact (only the payload changes, not the structure) → no API rejections from orphaned pairs.
- Atomic session rewrite: `.tmp` + rename, same pattern as `edit_file`.
- Single retry per iter via `continue`. If second overflow arrives, the clearing function returns 0 (nothing new to clear) and `chat_turn` bails with a clear message. No infinite loop.
- Deliberately omits: summarisation LLM call, proactive triggering via token count, manual `/compact`, custom prompt config. Lower lobotomy risk by design — nothing is summarised, just replaced with a `[cleared]` placeholder the model can re-fetch from.

### 0.7 — Distribution — done (except Homebrew formula)

Done:
- `config.def.h` → Makefile auto-copies to `config.h` on first build; user edits and rebuilds
- `make install` / `make uninstall` with `PREFIX` override (default `/usr/local`)
- Portable Makefile: macOS uses `-Wl,-dead_strip`; Linux uses `-Wl,--gc-sections`; `strip` post-build on both
- `README.md` with per-distro build instructions (macOS / Debian / Alpine / Arch)
- `patches/` directory with `README.md` documenting the planned diffs
- `.gitignore` for `config.h`, `tiny_c` binary, session file

Pending:
- Homebrew formula — requires a public tarball/git URL. Ship after first published release. Build is `make && sudo make install` today (zero runtime deps).

BearSSL replaced libcurl — zero link-time deps beyond libc. Static musl single-binary builds are now viable.

## Non-goals (forever)

- Slash commands (skills cover this; user-triggered templates belong in shell aliases or the one-shot `tiny_c "..."` mode)
- Hooks (PreToolUse/PostToolUse/UserPromptSubmit/Stop — fork/pipe + JSON plumbing costs ~160 LOC and invites matcher/async feature creep; shell wrappers around `tiny_c` cover the real use cases)
- TUI / curses / ANSI painting beyond `\x1b[2m` dimming
- MCP client
- Sessions beyond per-cwd JSONL
- Summarisation-based compaction (we do tool-result clearing in 0.6; summarisation adds lobotomy risk — drops specifics like line numbers, regex patterns, and implicit reasoning — for marginal gain over clearing. Ships as `patches/add-summarise.diff` if someone really wants it.)
- Rule `alwaysApply: false` and per-turn dynamic activation — we support `paths: [glob]` per-session conditional loading, which covers the real "am I in a C# repo" case. Per-turn activation costs ~50 LOC with marginal behavioural gain.
- Multi-provider abstraction layer (env-var switching is enough)
- Runtime plugin loading
- Auto-update
- Telemetry
- Web UI / remote server mode
- Parallel tool calls (sequential is fine at this scope)
- Thinking block reasoning in the tool loop

## Dependencies

- `libc` — baseline.
- `BearSSL` — vendored at `bearssl/`, statically linked. Public domain. Regenerate `ca.h` from the system CA bundle via `make ca.h`.
- `tinyjson.h` — hand-rolled, in-tree. No external JSON lib.

Zero runtime deps beyond libc.

## Distribution

- Source: `main.c` + `tinyjson.h` + `config.def.h` + `Makefile` + `ca.h` + `bearssl/` (vendored) + `README` + `ROADMAP.md`
- Tarball or `git clone`
- Build: `make` (first build copies `config.def.h` to `config.h` and builds BearSSL)
- Install: `sudo make install`
- Remove: `sudo make uninstall` (or `rm /usr/local/bin/sa`)

## What this explicitly is not

- Not a Claude Code replacement.
- Not a framework. No extension points beyond `config.h` and patches.
- Not a library. `main.c` is the product.
- Not a service. One-shot invocation, exits.

## Naming

Name is `sa` (simple/suckless agent). Two letters, Unix tool category.
