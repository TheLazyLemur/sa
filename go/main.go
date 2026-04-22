package main

import (
	"bufio"
	"bytes"
	"cmp"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

type M = map[string]any

const (
	base         = "You are a concise assistant. Use the shell tool for system questions. Keep replies short."
	session      = ".tiny_c_session.jsonl"
	maxIt        = 60
	maxBlock     = 1 << 20
	maxShell     = 1 << 20
	stallDelay   = 30 * time.Second
	shellTimeout = 5 * time.Minute
)

var (
	errContext = errors.New("context overflow")
	httpClient = &http.Client{Transport: &http.Transport{
		DialContext:           (&net.Dialer{Timeout: 10 * time.Second}).DialContext,
		ResponseHeaderTimeout: 30 * time.Second,
	}}
)

type blk struct{ typ, id, name, text, partial string }
type skill struct{ name, desc, path string }
type rule struct {
	path, body string
	globs      []string
}

func appendCap(s *string, add string) string {
	room := maxBlock - len(*s)
	if room <= 0 {
		return ""
	}
	if len(add) > room {
		return add[:room]
	}
	return add
}

func applyEvent(bs *[]*blk, ev M) {
	at := func(i int) *blk {
		for len(*bs) <= i {
			*bs = append(*bs, &blk{})
		}
		return (*bs)[i]
	}
	idx, _ := ev["index"].(float64)
	switch ev["type"] {
	case "content_block_start":
		b := at(int(idx))
		cb, _ := ev["content_block"].(M)
		b.typ, _ = cb["type"].(string)
		b.id, _ = cb["id"].(string)
		b.name, _ = cb["name"].(string)
	case "content_block_delta":
		b := at(int(idx))
		d, _ := ev["delta"].(M)
		if s, ok := d["text"].(string); ok {
			take := appendCap(&b.text, s)
			b.text += take
			fmt.Print(take)
		}
		if s, ok := d["partial_json"].(string); ok {
			b.partial += appendCap(&b.partial, s)
		}
	case "error":
		e, _ := ev["error"].(M)
		m, _ := e["message"].(string)
		fmt.Fprintf(os.Stderr, "\n[api error: %s]\n", m)
	}
}

func doTurn(parent context.Context, url, key string, body []byte) ([]*blk, error) {
	ctx, cancel := context.WithCancel(parent)
	defer cancel()
	r, _ := http.NewRequestWithContext(ctx, "POST", url, bytes.NewReader(body))
	for k, v := range map[string]string{"content-type": "application/json", "anthropic-version": "2023-06-01", "accept": "text/event-stream", "x-api-key": key} {
		r.Header.Set(k, v)
	}
	resp, err := httpClient.Do(r)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		b, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		var j struct {
			Error struct{ Message string } `json:"error"`
		}
		json.Unmarshal(b, &j)
		msg := j.Error.Message
		fmt.Fprintf(os.Stderr, "\napi status %d\n", resp.StatusCode)
		if msg != "" {
			fmt.Fprintf(os.Stderr, "error: %s\n", msg)
		}
		if resp.StatusCode == 400 && (strings.Contains(msg, "context") || strings.Contains(msg, "token") || strings.Contains(msg, "length")) {
			return nil, errContext
		}
		return nil, fmt.Errorf("api %d", resp.StatusCode)
	}
	var bs []*blk
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 1<<16), 4<<20)
	dbg := os.Getenv("TINY_DEBUG") != ""
	stall := time.AfterFunc(stallDelay, cancel)
	defer stall.Stop()
	for sc.Scan() {
		stall.Reset(stallDelay)
		l := sc.Text()
		if dbg {
			fmt.Fprintf(os.Stderr, "\x1b[2m%s\x1b[0m\n", l)
		}
		if !strings.HasPrefix(l, "data:") {
			continue
		}
		var ev M
		if json.Unmarshal([]byte(strings.TrimPrefix(strings.TrimPrefix(l, "data:"), " ")), &ev) != nil {
			continue
		}
		applyEvent(&bs, ev)
	}
	return bs, sc.Err()
}

type capW struct {
	buf   []byte
	max   int
	trunc bool
}

func (c *capW) Write(p []byte) (int, error) {
	if len(c.buf) < c.max {
		room := c.max - len(c.buf)
		if len(p) <= room {
			c.buf = append(c.buf, p...)
		} else {
			c.buf = append(c.buf, p[:room]...)
			c.trunc = true
		}
	} else {
		c.trunc = true
	}
	return len(p), nil
}

func shellRun(parent context.Context, cmd string) string {
	ctx, cancel := context.WithTimeout(parent, shellTimeout)
	defer cancel()
	w := &capW{max: maxShell}
	c := exec.CommandContext(ctx, "sh", "-c", cmd)
	c.Stdout = w
	c.Stderr = w
	err := c.Run()
	s := string(w.buf)
	if s == "" {
		s = "(no output)"
	}
	if w.trunc {
		s += "\n[output truncated at 1MB]"
	}
	code := 0
	switch {
	case err == nil:
	case errors.Is(ctx.Err(), context.DeadlineExceeded):
		code = -1
		s += "\n[timed out after 5m]"
	case errors.Is(parent.Err(), context.Canceled):
		code = -1
		s += "\n[cancelled]"
	default:
		if ee, ok := err.(*exec.ExitError); ok {
			code = ee.ExitCode()
		} else {
			return "error: shell: " + err.Error()
		}
	}
	return fmt.Sprintf("%s\n[exit %d]", s, code)
}

func readRun(path string, off, lim int) string {
	if lim <= 0 {
		lim = 2000
	}
	off = max(off, 1)
	f, err := os.Open(path)
	if err != nil {
		return "error: " + err.Error()
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<16), 1<<20)
	var o strings.Builder
	ln, n := 0, 0
	for sc.Scan() {
		ln++
		if bytes.IndexByte(sc.Bytes(), 0) >= 0 {
			return "error: binary file (NUL byte)"
		}
		if ln < off {
			continue
		}
		if n >= lim || o.Len() > 256<<10 {
			break
		}
		fmt.Fprintf(&o, "%6d\t%s\n", ln, sc.Text())
		n++
	}
	if n == 0 && ln == 0 {
		return "(empty file)"
	}
	if n == 0 {
		return fmt.Sprintf("error: offset %d beyond end (%d lines)", off, ln)
	}
	return o.String()
}

func atomicWrite(p string, d []byte) error {
	dir := filepath.Dir(p)
	f, err := os.CreateTemp(dir, filepath.Base(p)+".*")
	if err != nil {
		return err
	}
	tmp := f.Name()
	_, werr := f.Write(d)
	if cerr := f.Close(); werr == nil {
		werr = cerr
	}
	if werr != nil {
		os.Remove(tmp)
		return werr
	}
	if err := os.Rename(tmp, p); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

func editRun(path, oldStr, newStr string) string {
	out := []byte(newStr)
	if oldStr != "" {
		body, err := os.ReadFile(path)
		if err != nil {
			return "error: " + err.Error()
		}
		if len(body) > 4<<20 {
			return "error: file >4MB"
		}
		o := []byte(oldStr)
		switch bytes.Count(body, o) {
		case 0:
			return "error: old_string not found"
		case 1:
			out = bytes.Replace(body, o, []byte(newStr), 1)
		default:
			return "error: old_string matched multiple times"
		}
	}
	if err := atomicWrite(path, out); err != nil {
		return "error: " + err.Error()
	}
	return fmt.Sprintf("wrote %d bytes to %s", len(out), path)
}

func sessionAppend(m M) {
	f, err := os.OpenFile(session, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		return
	}
	defer f.Close()
	json.NewEncoder(f).Encode(m)
}

func sessionRewrite(msgs []M) error {
	f, err := os.CreateTemp(".", session+".*")
	if err != nil {
		return err
	}
	tmp := f.Name()
	enc := json.NewEncoder(f)
	var encErr error
	for _, m := range msgs {
		if e := enc.Encode(m); e != nil {
			encErr = e
			break
		}
	}
	if cerr := f.Close(); encErr == nil {
		encErr = cerr
	}
	if encErr != nil {
		os.Remove(tmp)
		return encErr
	}
	if err := os.Rename(tmp, session); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

func sessionLoad() (out []M) {
	data, err := os.ReadFile(session)
	if err != nil {
		return nil
	}
	for _, line := range strings.Split(string(data), "\n") {
		if line == "" {
			continue
		}
		var m M
		if json.Unmarshal([]byte(line), &m) == nil {
			out = append(out, m)
		}
	}
	return
}

func compactClear(msgs *[]M, keepLast int) int {
	cleared := 0
	keepFrom := len(*msgs) - keepLast
	if keepFrom < 0 {
		keepFrom = 0
	}
	visit := func(item M) {
		if item["type"] != "tool_result" {
			return
		}
		s, _ := item["content"].(string)
		if len(s) > 10 && s != "[cleared]" {
			item["content"] = "[cleared]"
			cleared++
		}
	}
	for i := 0; i < keepFrom; i++ {
		switch c := (*msgs)[i]["content"].(type) {
		case []M:
			for _, it := range c {
				visit(it)
			}
		case []any:
			for _, it := range c {
				if m, ok := it.(M); ok {
					visit(m)
				}
			}
		}
	}
	return cleared
}

func frontmatter(path string) (kv map[string]string, ok bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return
	}
	lines := strings.Split(string(data), "\n")
	if len(lines) < 2 || strings.TrimSpace(lines[0]) != "---" {
		return
	}
	kv = map[string]string{}
	cur := ""
	for _, l := range lines[1:] {
		t := strings.TrimSpace(l)
		if t == "---" {
			break
		}
		indent := len(l) > 0 && (l[0] == ' ' || l[0] == '\t')
		if indent && cur != "" && t != "" {
			if kv[cur] != "" {
				kv[cur] += " "
			}
			kv[cur] += t
			continue
		}
		j := strings.Index(t, ":")
		if j < 0 {
			cur = ""
			continue
		}
		k, v := strings.TrimSpace(t[:j]), strings.Trim(strings.TrimSpace(t[j+1:]), `"'`)
		cur = k
		if len(v) > 0 && (v[0] == '>' || v[0] == '|') {
			kv[k] = ""
		} else {
			kv[k] = v
		}
	}
	return kv, kv["name"] != ""
}

func scanSkills(dir string) (out []skill) {
	es, _ := os.ReadDir(dir)
	for _, e := range es {
		if strings.HasPrefix(e.Name(), ".") {
			continue
		}
		p := filepath.Join(dir, e.Name())
		src := p
		if e.IsDir() {
			src = filepath.Join(p, "SKILL.md")
		} else if !strings.HasSuffix(e.Name(), ".md") {
			continue
		}
		if kv, ok := frontmatter(src); ok {
			abs, _ := filepath.Abs(src)
			out = append(out, skill{kv["name"], kv["description"], abs})
		}
	}
	return
}

func parseRule(path string) rule {
	b, _ := os.ReadFile(path)
	r := rule{path: path, body: string(b)}
	if !strings.HasPrefix(r.body, "---") {
		return r
	}
	inP := false
	for i, l := range strings.Split(r.body, "\n") {
		if i == 0 {
			continue
		}
		t := strings.TrimSpace(l)
		if t == "---" {
			break
		}
		if len(l) > 0 && l[0] != ' ' && l[0] != '\t' {
			inP = strings.HasPrefix(t, "paths:")
			continue
		}
		if !inP || !strings.HasPrefix(t, "-") {
			continue
		}
		v := strings.Trim(strings.TrimSpace(t[1:]), `"' `+"\r")
		if v != "" {
			r.globs = append(r.globs, v)
		}
	}
	return r
}

func scanRules(dir string) (out []rule) {
	es, _ := os.ReadDir(dir)
	for _, e := range es {
		n := e.Name()
		if strings.HasPrefix(n, ".") || !strings.HasSuffix(n, ".md") {
			continue
		}
		p := filepath.Join(dir, n)
		fi, err := os.Stat(p)
		if err != nil || !fi.Mode().IsRegular() || fi.Size() > 64<<10 {
			continue
		}
		out = append(out, parseRule(p))
	}
	return
}

func ruleFires(r rule) bool {
	if len(r.globs) == 0 {
		return true
	}
	stop := errors.New("stop")
	for _, g := range r.globs {
		g = strings.TrimPrefix(g, "**/")
		found := false
		filepath.Walk(".", func(p string, fi os.FileInfo, err error) error {
			if err != nil {
				return nil
			}
			if fi.IsDir() {
				if fi.Name() == ".git" {
					return filepath.SkipDir
				}
				return nil
			}
			if ok, _ := filepath.Match(g, fi.Name()); ok {
				found = true
				return stop
			}
			return nil
		})
		if found {
			return true
		}
	}
	return false
}

func sysPrompt(sks []skill, rs []rule) string {
	var b strings.Builder
	b.WriteString(base)
	home, _ := os.UserHomeDir()
	loaded := 0
	for _, s := range [][2]string{{filepath.Join(home, ".claude", "CLAUDE.md"), "user's private global instructions for all projects"}, {"./CLAUDE.md", "project instructions"}} {
		if d, err := os.ReadFile(s[0]); err == nil {
			fmt.Fprintf(&b, "\n\nContents of %s (%s):\n\n%s", s[0], s[1], d)
			loaded++
		}
	}
	if loaded > 0 {
		fmt.Fprintf(os.Stderr, "[loaded %d CLAUDE.md]\n", loaded)
	}
	for _, r := range rs {
		if ruleFires(r) {
			fmt.Fprintf(&b, "\n\nContents of %s (rule):\n\n%s", r.path, r.body)
		}
	}
	if len(sks) > 0 {
		esc := strings.NewReplacer("&", "&amp;", "<", "&lt;", ">", "&gt;")
		b.WriteString("\n\nTo use a skill, first read its SKILL.md (via `cat <path>`) to learn its instructions, then follow them.\n\n<skills>\n")
		for _, s := range sks {
			fmt.Fprintf(&b, "  <skill>\n    <name>%s</name>\n    <description>%s</description>\n    <path>%s</path>\n  </skill>\n", esc.Replace(s.name), esc.Replace(s.desc), esc.Replace(s.path))
		}
		b.WriteString("</skills>")
	}
	return b.String()
}

var toolDefs = []M{
	{"name": "shell", "description": "Run a shell command via /bin/sh -c. Returns combined stdout+stderr and exit code.",
		"input_schema": M{"type": "object", "properties": M{"cmd": M{"type": "string"}}, "required": []string{"cmd"}}},
	{"name": "read_file", "description": "Read a text file. Returns up to 'limit' lines starting from 'offset' (1-indexed), each prefixed with line number + tab. Default limit 2000. Hard cap 256KB.",
		"input_schema": M{"type": "object", "properties": M{"path": M{"type": "string"}, "offset": M{"type": "integer"}, "limit": M{"type": "integer"}}, "required": []string{"path"}}},
	{"name": "edit_file", "description": "Edit a file. Empty old_string writes new_string as full contents. Non-empty requires unique match and replaces. Atomic .tmp+rename. 4MB cap.",
		"input_schema": M{"type": "object", "properties": M{"path": M{"type": "string"}, "old_string": M{"type": "string"}, "new_string": M{"type": "string"}}, "required": []string{"path", "old_string", "new_string"}}},
}

func dispatch(ctx context.Context, name string, in M) string {
	get := func(k string) (string, bool) { v, ok := in[k].(string); return v, ok }
	num := func(k string) int { v, _ := in[k].(float64); return int(v) }
	log := func(f string, a ...any) { fmt.Fprintf(os.Stderr, "\n\x1b[2m"+f+"\x1b[0m\n", a...) }
	switch name {
	case "shell":
		cmd, ok := get("cmd")
		if !ok || cmd == "" {
			return "error: shell requires cmd (string)"
		}
		log("[shell] %s", cmd)
		return shellRun(ctx, cmd)
	case "read_file":
		p, ok := get("path")
		if !ok || p == "" {
			return "error: read_file requires path (string)"
		}
		log("[read_file] %s", p)
		return readRun(p, num("offset"), num("limit"))
	case "edit_file":
		p, pok := get("path")
		if !pok || p == "" {
			return "error: edit_file requires path (string)"
		}
		o, ook := get("old_string")
		n, nok := get("new_string")
		if !ook || !nok {
			return "error: edit_file requires old_string and new_string (strings)"
		}
		mode := "replace"
		if o == "" {
			mode = "write"
		}
		log("[edit_file] %s (%s)", p, mode)
		return editRun(p, o, n)
	}
	return "error: unknown tool"
}

func chatTurn(ctx context.Context, url, key, model, sp string, msgs *[]M) int {
	for it := 0; it < maxIt; it++ {
		if ctx.Err() != nil {
			return 130
		}
		body, _ := json.Marshal(M{"model": model, "max_tokens": 4096, "system": sp, "messages": *msgs, "tools": toolDefs, "stream": true})
		bs, err := doTurn(ctx, url, key, body)
		fmt.Println()
		if errors.Is(err, errContext) {
			n := compactClear(msgs, 10)
			if n == 0 {
				fmt.Fprintln(os.Stderr, "[context overflow with nothing to compact; bailing]")
				return 1
			}
			fmt.Fprintf(os.Stderr, "[compacted: cleared %d tool result(s); retrying]\n", n)
			if err := sessionRewrite(*msgs); err != nil {
				fmt.Fprintf(os.Stderr, "[session rewrite failed: %v]\n", err)
			}
			continue
		}
		if errors.Is(err, context.Canceled) {
			fmt.Fprintln(os.Stderr, "\n[interrupted]")
			return 130
		}
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		var cont, res []M
		for _, b := range bs {
			if b.typ == "" {
				continue
			}
			o := M{"type": b.typ}
			switch b.typ {
			case "text":
				o["text"] = b.text
			case "tool_use":
				var in any = M{}
				if b.partial != "" {
					if err := json.Unmarshal([]byte(b.partial), &in); err != nil {
						fmt.Fprintf(os.Stderr, "\n[tool_use %s: invalid input JSON: %v]\n", b.id, err)
						in = M{}
					}
				}
				o["id"], o["name"], o["input"] = b.id, b.name, in
				inMap, _ := in.(M)
				if inMap == nil {
					inMap = M{}
				}
				res = append(res, M{"type": "tool_result", "tool_use_id": b.id, "content": dispatch(ctx, b.name, inMap)})
			}
			cont = append(cont, o)
		}
		a := M{"role": "assistant", "content": cont}
		*msgs = append(*msgs, a)
		sessionAppend(a)
		if len(res) == 0 {
			return 0
		}
		r := M{"role": "user", "content": res}
		*msgs = append(*msgs, r)
		sessionAppend(r)
	}
	fmt.Fprintln(os.Stderr, "\n[max tool iterations reached]")
	return 1
}

func main() {
	args := os.Args[1:]
	cont := false
	if len(args) > 0 && (args[0] == "-c" || args[0] == "--continue") {
		cont, args = true, args[1:]
	}
	key := os.Getenv("KIMI_TOKEN")
	if key == "" {
		fmt.Fprintln(os.Stderr, "set KIMI_TOKEN")
		os.Exit(1)
	}
	bu := cmp.Or(os.Getenv("KIMI_BASE_URL"), "https://api.kimi.com/coding")
	model := cmp.Or(os.Getenv("MODEL"), "kimi-for-coding")
	url := strings.TrimRight(bu, "/") + "/v1/messages"

	var sks []skill
	var rs []rule
	home, _ := os.UserHomeDir()
	for _, d := range []string{filepath.Join(home, ".claude"), "./.claude"} {
		sks = append(sks, scanSkills(filepath.Join(d, "skills"))...)
		rs = append(rs, scanRules(filepath.Join(d, "rules"))...)
	}
	if len(sks)+len(rs) > 0 {
		fmt.Fprintf(os.Stderr, "[loaded %d skill(s), %d rule(s)]\n", len(sks), len(rs))
	}

	sp := sysPrompt(sks, rs)
	var msgs []M
	if cont {
		msgs = sessionLoad()
		fmt.Fprintf(os.Stderr, "[resumed %d messages]\n", len(msgs))
	}

	run := func(text string) int {
		um := M{"role": "user", "content": []M{{"type": "text", "text": text}}}
		msgs = append(msgs, um)
		sessionAppend(um)
		ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT)
		defer stop()
		return chatTurn(ctx, url, key, model, sp, &msgs)
	}

	if len(args) > 0 {
		os.Exit(run(strings.Join(args, " ")))
	}

	fi, _ := os.Stdin.Stat()
	ttyOn := fi != nil && fi.Mode()&os.ModeCharDevice != 0
	if ttyOn {
		fmt.Fprintf(os.Stderr, "tiny_c %s — Ctrl+D to exit, Ctrl+C to interrupt\n", model)
	}
	rd := bufio.NewReader(os.Stdin)
	for {
		if ttyOn {
			fmt.Fprint(os.Stderr, "\n\x1b[1;36m› \x1b[0m")
		}
		l, err := rd.ReadString('\n')
		if err != nil {
			if ttyOn {
				fmt.Fprintln(os.Stderr)
			}
			break
		}
		l = strings.TrimRight(l, "\r\n")
		if l == "" {
			continue
		}
		if run(l) == 1 {
			os.Exit(1)
		}
	}
}
