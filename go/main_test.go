package main

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func tempFile(t *testing.T, name, content string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(p, []byte(content), 0644); err != nil {
		t.Fatalf("setup: %v", err)
	}
	return p
}

func chdirTemp(t *testing.T) {
	t.Helper()
	dir := t.TempDir()
	cwd, _ := os.Getwd()
	t.Cleanup(func() { os.Chdir(cwd) })
	if err := os.Chdir(dir); err != nil {
		t.Fatalf("chdir: %v", err)
	}
}

func TestCapW_AppendsBelowMax(t *testing.T) {
	// given
	// ... a cap writer with max 100
	w := &capW{max: 100}

	// when
	// ... writing 50 bytes
	n, _ := w.Write(make([]byte, 50))

	// then
	// ... all 50 bytes buffered, no truncation flag set
	if n != 50 {
		t.Fatalf("Write n: want 50, got %d", n)
	}
	if len(w.buf) != 50 {
		t.Fatalf("buf: want 50, got %d", len(w.buf))
	}
	if w.trunc {
		t.Fatalf("trunc should be false")
	}
}

func TestCapW_TruncatesAtMax(t *testing.T) {
	// given
	// ... a cap writer with max 10
	w := &capW{max: 10}

	// when
	// ... writing 25 bytes in one call
	n, _ := w.Write(make([]byte, 25))

	// then
	// ... 25 reported to caller, 10 buffered, trunc flag set
	if n != 25 {
		t.Fatalf("Write n: want 25, got %d", n)
	}
	if len(w.buf) != 10 {
		t.Fatalf("buf: want 10, got %d", len(w.buf))
	}
	if !w.trunc {
		t.Fatalf("trunc should be true")
	}
}

func TestCapW_TruncatesAcrossMultipleWrites(t *testing.T) {
	// given
	// ... a cap writer with max 5
	w := &capW{max: 5}

	// when
	// ... two writes totalling 8 bytes
	w.Write([]byte("abc"))
	w.Write([]byte("defgh"))

	// then
	// ... buffer is "abcde" and trunc flag set
	if string(w.buf) != "abcde" {
		t.Fatalf("buf: got %q", w.buf)
	}
	if !w.trunc {
		t.Fatalf("trunc should be true")
	}
}

func TestAppendCap_PassesThroughBelowMax(t *testing.T) {
	// given
	// ... a string well below maxBlock
	s := "abc"

	// when
	// ... asking what room is available for "xyz"
	got := appendCap(&s, "xyz")

	// then
	// ... the full "xyz" is returned (no truncation)
	if got != "xyz" {
		t.Fatalf("got %q, want xyz", got)
	}
}

func TestAppendCap_TruncatesToRoom(t *testing.T) {
	// given
	// ... a string already at maxBlock-2
	s := strings.Repeat("a", maxBlock-2)

	// when
	// ... asking for "xxxxx" (5 bytes)
	got := appendCap(&s, "xxxxx")

	// then
	// ... only the first 2 bytes fit
	if got != "xx" {
		t.Fatalf("got %q, want xx", got)
	}
}

func TestAppendCap_ReturnsEmptyAtMax(t *testing.T) {
	// given
	// ... a string already at maxBlock
	s := strings.Repeat("a", maxBlock)

	// when
	// ... asking for any addition
	got := appendCap(&s, "x")

	// then
	// ... empty string (no room)
	if got != "" {
		t.Fatalf("got %q, want empty", got)
	}
}

func TestApplyEvent_AccumulatesTextAndCapturesMetadata(t *testing.T) {
	// given
	// ... a fresh block slice and a start event followed by two text_delta events
	bs := []*blk{}
	start := M{"type": "content_block_start", "index": 0.0, "content_block": M{"type": "text", "id": "a", "name": "n"}}
	d1 := M{"type": "content_block_delta", "index": 0.0, "delta": M{"type": "text_delta", "text": "hel"}}
	d2 := M{"type": "content_block_delta", "index": 0.0, "delta": M{"type": "text_delta", "text": "lo"}}

	// when
	// ... each event is applied in order
	applyEvent(&bs, start)
	applyEvent(&bs, d1)
	applyEvent(&bs, d2)

	// then
	// ... the block has the accumulated text and retained metadata
	if len(bs) != 1 {
		t.Fatalf("want 1 block, got %d", len(bs))
	}
	if bs[0].typ != "text" || bs[0].id != "a" || bs[0].name != "n" {
		t.Fatalf("metadata: %+v", bs[0])
	}
	if bs[0].text != "hello" {
		t.Fatalf("text: got %q", bs[0].text)
	}
}

func TestApplyEvent_AccumulatesPartialJSON(t *testing.T) {
	// given
	// ... a start event for index 2 and two input_json_delta events
	bs := []*blk{}
	applyEvent(&bs, M{"type": "content_block_start", "index": 2.0, "content_block": M{"type": "tool_use", "id": "t1", "name": "shell"}})

	// when
	// ... two input_json_delta fragments arrive
	applyEvent(&bs, M{"type": "content_block_delta", "index": 2.0, "delta": M{"type": "input_json_delta", "partial_json": `{"cmd":`}})
	applyEvent(&bs, M{"type": "content_block_delta", "index": 2.0, "delta": M{"type": "input_json_delta", "partial_json": `"ls"}`}})

	// then
	// ... block index 2 has the concatenated partial JSON and indices 0 and 1 are empty placeholders
	if len(bs) != 3 {
		t.Fatalf("want 3 blocks, got %d", len(bs))
	}
	if bs[2].partial != `{"cmd":"ls"}` {
		t.Fatalf("partial: got %q", bs[2].partial)
	}
}

func TestCompactClear_ReplacesOldToolResults(t *testing.T) {
	// given
	// ... five messages each carrying a long tool_result
	var msgs []M
	for range 5 {
		msgs = append(msgs, M{
			"role":    "user",
			"content": []M{{"type": "tool_result", "tool_use_id": "x", "content": "some_long_result_text_here"}},
		})
	}

	// when
	// ... compactClear keeps the last two
	n := compactClear(&msgs, 2)

	// then
	// ... three earlier results are cleared and the last two are untouched
	if n != 3 {
		t.Fatalf("cleared: want 3, got %d", n)
	}
	for i := 0; i < 3; i++ {
		items := msgs[i]["content"].([]M)
		if items[0]["content"] != "[cleared]" {
			t.Fatalf("msg %d not cleared: %v", i, items[0]["content"])
		}
	}
	items := msgs[3]["content"].([]M)
	if items[0]["content"] != "some_long_result_text_here" {
		t.Fatalf("msg 3 altered: %v", items[0]["content"])
	}
}

func TestCompactClear_SkipsShortAndAlreadyCleared(t *testing.T) {
	// given
	// ... a short result, an already-cleared result, a long result, and a text block
	msgs := []M{
		{"role": "user", "content": []M{{"type": "tool_result", "content": "short"}}},
		{"role": "user", "content": []M{{"type": "tool_result", "content": "[cleared]"}}},
		{"role": "user", "content": []M{{"type": "tool_result", "content": "long_enough_result_text"}}},
		{"role": "assistant", "content": []M{{"type": "text", "text": "hi"}}},
	}

	// when
	// ... compactClear runs with keepLast=0 so every message is eligible
	n := compactClear(&msgs, 0)

	// then
	// ... only the one long non-cleared tool_result is counted
	if n != 1 {
		t.Fatalf("want 1, got %d", n)
	}
}

func TestCompactClear_HandlesLoadedAnyContent(t *testing.T) {
	// given
	// ... a message round-tripped through json.Unmarshal so content is []any
	raw := `{"role":"user","content":[{"type":"tool_result","tool_use_id":"x","content":"long_enough_content"}]}`
	var m M
	if err := json.Unmarshal([]byte(raw), &m); err != nil {
		t.Fatalf("setup: %v", err)
	}
	msgs := []M{m}

	// when
	// ... compactClear runs with keepLast=0
	n := compactClear(&msgs, 0)

	// then
	// ... the []any branch triggers and content is replaced
	if n != 1 {
		t.Fatalf("want 1, got %d", n)
	}
	items := msgs[0]["content"].([]any)
	got := items[0].(M)["content"]
	if got != "[cleared]" {
		t.Fatalf("content: got %v", got)
	}
}

func TestFrontmatter_ParsesInlineFields(t *testing.T) {
	// given
	// ... a skill file with a single-line name and description
	p := tempFile(t, "f.md", "---\nname: example\ndescription: a basic skill\n---\nbody\n")

	// when
	// ... frontmatter parses the header
	kv, ok := frontmatter(p)

	// then
	// ... both fields are returned
	if !ok {
		t.Fatalf("ok should be true")
	}
	if kv["name"] != "example" || kv["description"] != "a basic skill" {
		t.Fatalf("kv: %+v", kv)
	}
}

func TestFrontmatter_FoldsMultilineDescription(t *testing.T) {
	// given
	// ... a skill file with a > folded description
	p := tempFile(t, "f.md", "---\nname: x\ndescription: >\n  first line\n  second line\n---\n")

	// when
	// ... frontmatter parses the header
	kv, ok := frontmatter(p)

	// then
	// ... description lines are joined with a single space
	if !ok {
		t.Fatalf("ok should be true")
	}
	if kv["description"] != "first line second line" {
		t.Fatalf("description: %q", kv["description"])
	}
}

func TestFrontmatter_RejectsMissingHeader(t *testing.T) {
	// given
	// ... a file with no --- delimiters
	p := tempFile(t, "f.md", "just a body\n")

	// when
	// ... frontmatter is asked to parse
	_, ok := frontmatter(p)

	// then
	// ... ok is false
	if ok {
		t.Fatalf("ok should be false")
	}
}

func TestParseRule_ExtractsPathsList(t *testing.T) {
	// given
	// ... a rule with a paths: list in its YAML header
	p := tempFile(t, "r.md", "---\npaths:\n  - \"*.go\"\n  - \"**/*.py\"\n---\nrule body\n")

	// when
	// ... parseRule reads it
	r := parseRule(p)

	// then
	// ... globs are captured verbatim (prefix stripping happens in ruleFires)
	if len(r.globs) != 2 || r.globs[0] != "*.go" || r.globs[1] != "**/*.py" {
		t.Fatalf("globs: %+v", r.globs)
	}
}

func TestParseRule_NoFrontmatterHasNoGlobs(t *testing.T) {
	// given
	// ... a rule with no frontmatter
	p := tempFile(t, "r.md", "plain rule body\n")

	// when
	// ... parseRule reads it
	r := parseRule(p)

	// then
	// ... no globs captured and body retained
	if len(r.globs) != 0 {
		t.Fatalf("globs: %+v", r.globs)
	}
	if !strings.Contains(r.body, "plain rule") {
		t.Fatalf("body: %q", r.body)
	}
}

func TestEditRun_WritesNewFile(t *testing.T) {
	// given
	// ... a nonexistent path inside a tempdir
	p := filepath.Join(t.TempDir(), "new.txt")

	// when
	// ... editRun with empty old_string writes the full contents
	out := editRun(p, "", "hello")

	// then
	// ... file is created with "hello" and a success message is returned
	if !strings.HasPrefix(out, "wrote ") {
		t.Fatalf("result: %q", out)
	}
	body, _ := os.ReadFile(p)
	if string(body) != "hello" {
		t.Fatalf("content: %q", body)
	}
}

func TestEditRun_ReplacesUniqueMatch(t *testing.T) {
	// given
	// ... a file containing the target substring exactly once
	p := tempFile(t, "f.txt", "alpha beta gamma")

	// when
	// ... editRun replaces "beta" with "delta"
	out := editRun(p, "beta", "delta")

	// then
	// ... file is updated and success message is returned
	if !strings.HasPrefix(out, "wrote ") {
		t.Fatalf("result: %q", out)
	}
	body, _ := os.ReadFile(p)
	if string(body) != "alpha delta gamma" {
		t.Fatalf("content: %q", body)
	}
}

func TestEditRun_RejectsMultipleMatches(t *testing.T) {
	// given
	// ... a file where the target appears more than once
	p := tempFile(t, "f.txt", "x x")

	// when
	// ... editRun attempts the replace
	out := editRun(p, "x", "y")

	// then
	// ... error is returned and the file is left unchanged
	if !strings.HasPrefix(out, "error: old_string matched") {
		t.Fatalf("result: %q", out)
	}
	body, _ := os.ReadFile(p)
	if string(body) != "x x" {
		t.Fatalf("file altered: %q", body)
	}
}

func TestEditRun_RejectsMissingMatch(t *testing.T) {
	// given
	// ... a file that does not contain the target
	p := tempFile(t, "f.txt", "hello")

	// when
	// ... editRun attempts the replace
	out := editRun(p, "missing", "x")

	// then
	// ... error is returned
	if !strings.HasPrefix(out, "error: old_string not found") {
		t.Fatalf("result: %q", out)
	}
}

func TestReadRun_ReturnsLineNumberedContent(t *testing.T) {
	// given
	// ... a file with three lines
	p := tempFile(t, "f.txt", "a\nb\nc\n")

	// when
	// ... readRun uses default offset and limit
	out := readRun(p, 0, 0)

	// then
	// ... each line is prefixed with its 1-based line number and a tab
	want := "     1\ta\n     2\tb\n     3\tc\n"
	if out != want {
		t.Fatalf("got %q, want %q", out, want)
	}
}

func TestReadRun_OffsetBeyondEnd(t *testing.T) {
	// given
	// ... a one-line file
	p := tempFile(t, "f.txt", "only\n")

	// when
	// ... readRun is called with an offset past the end
	out := readRun(p, 99, 10)

	// then
	// ... the error message names the offset and line count
	if !strings.HasPrefix(out, "error: offset 99") {
		t.Fatalf("result: %q", out)
	}
}

func TestReadRun_DetectsBinaryFile(t *testing.T) {
	// given
	// ... a file containing a NUL byte
	p := tempFile(t, "bin", "A\x00B")

	// when
	// ... readRun is called on it
	out := readRun(p, 0, 0)

	// then
	// ... the binary-file error is returned
	if !strings.Contains(out, "binary file") {
		t.Fatalf("result: %q", out)
	}
}

func TestReadRun_EmptyFile(t *testing.T) {
	// given
	// ... an empty file
	p := tempFile(t, "f.txt", "")

	// when
	// ... readRun is called
	out := readRun(p, 0, 0)

	// then
	// ... the empty-file sentinel is returned
	if out != "(empty file)" {
		t.Fatalf("result: %q", out)
	}
}

func TestAtomicWrite_ReplacesContents(t *testing.T) {
	// given
	// ... a path with an existing file
	dir := t.TempDir()
	p := filepath.Join(dir, "f.txt")
	os.WriteFile(p, []byte("old"), 0644)

	// when
	// ... atomicWrite writes new content
	err := atomicWrite(p, []byte("new"))

	// then
	// ... no error, file replaced, no temp files left over
	if err != nil {
		t.Fatalf("err: %v", err)
	}
	body, _ := os.ReadFile(p)
	if string(body) != "new" {
		t.Fatalf("content: %q", body)
	}
	entries, _ := os.ReadDir(dir)
	if len(entries) != 1 {
		t.Fatalf("leftover files: %v", entries)
	}
}

func TestSessionRewrite_WritesOneLinePerMessage(t *testing.T) {
	// given
	// ... a tempdir as cwd with two messages to persist
	chdirTemp(t)
	msgs := []M{{"role": "user", "content": "hi"}, {"role": "assistant", "content": "hello"}}

	// when
	// ... sessionRewrite is called
	err := sessionRewrite(msgs)

	// then
	// ... each message is a valid JSON line and only the session file remains
	if err != nil {
		t.Fatalf("err: %v", err)
	}
	data, _ := os.ReadFile(session)
	lines := strings.Split(strings.TrimRight(string(data), "\n"), "\n")
	if len(lines) != 2 {
		t.Fatalf("lines: %d", len(lines))
	}
	for i, l := range lines {
		var m M
		if err := json.Unmarshal([]byte(l), &m); err != nil {
			t.Fatalf("line %d invalid: %v", i, err)
		}
	}
}

func TestSessionAppendLoad_Roundtrip(t *testing.T) {
	// given
	// ... a fresh cwd and two appends
	chdirTemp(t)
	sessionAppend(M{"role": "user", "content": "one"})
	sessionAppend(M{"role": "assistant", "content": "two"})

	// when
	// ... sessionLoad reads the file back
	got := sessionLoad()

	// then
	// ... both messages are returned in order
	if len(got) != 2 {
		t.Fatalf("len: %d", len(got))
	}
	if got[0]["content"] != "one" || got[1]["content"] != "two" {
		t.Fatalf("contents: %+v", got)
	}
}

func TestDispatch_ErrorsOnMissingCmd(t *testing.T) {
	// given
	// ... an input map missing the cmd field
	in := M{}

	// when
	// ... dispatch targets the shell tool
	out := dispatch(context.Background(), "shell", in)

	// then
	// ... a deterministic error message is returned
	if !strings.HasPrefix(out, "error: shell requires cmd") {
		t.Fatalf("result: %q", out)
	}
}

func TestDispatch_UnknownTool(t *testing.T) {
	// given
	// ... an empty input
	in := M{}

	// when
	// ... dispatch receives an unrecognised tool name
	out := dispatch(context.Background(), "does_not_exist", in)

	// then
	// ... the generic unknown-tool error is returned
	if out != "error: unknown tool" {
		t.Fatalf("result: %q", out)
	}
}

func TestShellRun_CapturesOutputAndExitCode(t *testing.T) {
	// given
	// ... a command that prints to stdout and exits zero
	ctx := context.Background()

	// when
	// ... shellRun executes it
	out := shellRun(ctx, "printf hi")

	// then
	// ... output is present and the exit 0 trailer is appended
	if !strings.Contains(out, "hi") {
		t.Fatalf("result: %q", out)
	}
	if !strings.HasSuffix(out, "[exit 0]") {
		t.Fatalf("result: %q", out)
	}
}

func TestShellRun_CancelledByParentContext(t *testing.T) {
	// given
	// ... a parent context that is already cancelled
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	// when
	// ... shellRun is asked to sleep
	start := time.Now()
	out := shellRun(ctx, "sleep 30")

	// then
	// ... the call returns quickly with a cancelled marker
	if time.Since(start) > 2*time.Second {
		t.Fatalf("took too long: %v", time.Since(start))
	}
	if !strings.Contains(out, "[cancelled]") {
		t.Fatalf("result: %q", out)
	}
}
