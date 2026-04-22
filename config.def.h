/* tiny_c compile-time configuration.
   Copy this to config.h and edit. The Makefile auto-copies on first build. */
#ifndef TINY_C_CONFIG_H
#define TINY_C_CONFIG_H

/* Base system prompt — prepended before CLAUDE.md, rules, skills. */
#define BASE_SYSTEM "You are a concise assistant. Use the shell tool for system questions. Keep replies short."

/* Session JSONL filename, written in CWD. */
#define SESSION_PATH ".tiny_c_session.jsonl"

/* Max skills loaded from ~/.claude/skills + ./.claude/skills combined. */
#define MAX_SKILLS 128

/* Max rules loaded from ~/.claude/rules + ./.claude/rules combined. */
#define MAX_RULES 64

/* Max path globs per rule's `paths:` frontmatter list. */
#define MAX_RULE_PATHS 8

/* Max concurrent content blocks in a single assistant response. */
#define MAX_BLOCKS 64

/* Max tool-call iterations per user message before forced exit. */
#define MAX_TOOL_ITER 60

/* Max SSE line buffer (bytes) — anti-DoS cap. */
#define MAX_SSE_LINE (1u << 20)

/* Max text/partial_json buffer per content block (bytes) — anti-DoS cap. */
#define MAX_BLOCK_SIZE (1u << 20)

#endif
