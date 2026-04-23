#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <curl/curl.h>
#include "tinyjson.h"
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/select.h>
#include "bearssl.h"
#include "ca.h"

#include "config.h"

static volatile sig_atomic_t g_interrupt = 0;
static int g_debug = 0;
static void on_sigint(int sig) { (void)sig; g_interrupt = 1; }

/* --- buf_t --- */
typedef struct { char *data; size_t len, cap; } buf_t;

static void buf_reserve(buf_t *b, size_t n) {
    if (b->cap >= n + 1) return;
    size_t c = b->cap ? b->cap : 64;
    while (c < n + 1) c *= 2;
    char *p = realloc(b->data, c);
    if (!p) abort();
    b->data = p;
    b->cap = c;
}
static void buf_append(buf_t *b, const char *s, size_t n) {
    buf_reserve(b, b->len + n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
static void buf_reset(buf_t *b) { b->len = 0; if (b->data) b->data[0] = 0; }
static void buf_free(buf_t *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

/* --- net --- */
static int tcp_connect(const char *host, const char *port, int timeout_sec) {
    struct addrinfo hints = {0}, *res = NULL, *rp;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0) { fcntl(fd, F_SETFL, flags); break; }
        if (errno != EINPROGRESS) { close(fd); fd = -1; continue; }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { timeout_sec, 0 };
        int sr = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (sr <= 0) { close(fd); fd = -1; continue; }
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err != 0) { close(fd); fd = -1; continue; }
        fcntl(fd, F_SETFL, flags);
        break;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    struct timeval rt = { 30, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &rt, sizeof rt);
    return fd;
}

static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n >= 0) return (int)n;
        if (errno == EINTR) continue;
        return -1;
    }
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = send(fd, buf, len, 0);
        if (n >= 0) return (int)n;
        if (errno == EINTR) continue;
        return -1;
    }
}

/* --- stream --- */
typedef struct {
    char type[32];
    char id[64];
    char name[64];
    buf_t text;
    buf_t partial;
} block_t;

typedef struct {
    block_t blocks[MAX_BLOCKS];
    int n;
    buf_t line;
    int line_dropped;
    char raw_head[4096];
    size_t raw_head_len;
} stream_state_t;

static size_t on_chunk(char *ptr, size_t size, size_t nmemb, void *userdata);

static int parse_url(const char *url, char *host, size_t host_sz, int *port, char *path, size_t path_sz) {
    if (strncmp(url, "https://", 8) != 0) return -1;
    const char *p = url + 8;
    const char *slash = strchr(p, '/');
    size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hostlen == 0 || hostlen >= host_sz) return -1;
    memcpy(host, p, hostlen);
    host[hostlen] = 0;
    *port = 443;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) return -1;
    }
    snprintf(path, path_sz, "%s", slash ? slash : "/");
    return 0;
}

/* read bytes from ssl until we see \r\n\r\n; returns 0 on ok, -1 on err.
   fills *status, and hdr[] with full header block (null-terminated). */
static int http_read_headers(br_sslio_context *ioc, int *status, char *hdr, size_t hdr_sz, int *chunked, long *clen) {
    size_t used = 0;
    int blank_line_found = 0;
    while (used + 1 < hdr_sz) {
        unsigned char c;
        int r = br_sslio_read(ioc, &c, 1);
        if (r <= 0) return -1;
        hdr[used++] = (char)c;
        if (used >= 4 && memcmp(hdr + used - 4, "\r\n\r\n", 4) == 0) { blank_line_found = 1; break; }
    }
    if (!blank_line_found) return -1;
    hdr[used] = 0;

    /* status line: "HTTP/1.1 NNN ..." */
    if (sscanf(hdr, "HTTP/1.%*d %d", status) != 1) return -1;

    /* scan for Transfer-Encoding and Content-Length (case-insensitive prefix) */
    *chunked = 0;
    *clen = -1;
    const char *line = hdr;
    while (line && *line) {
        const char *eol = strstr(line, "\r\n");
        if (!eol) break;
        if (strncasecmp(line, "transfer-encoding:", 18) == 0) {
            if (strstr(line, "chunked")) *chunked = 1;
        } else if (strncasecmp(line, "content-length:", 15) == 0) {
            *clen = strtol(line + 15, NULL, 10);
        }
        line = eol + 2;
    }
    return 0;
}

/* Streams body to cb(userdata, bytes, n). Returns 0 on ok, -1 on err, -2 on interrupt. */
static int http_stream_body(br_sslio_context *ioc, int chunked, long clen,
                            size_t (*cb)(char *, size_t, size_t, void *), void *userdata) {
    unsigned char buf[4096];
    if (chunked) {
        for (;;) {
            if (g_interrupt) return -2;
            /* read chunk size line */
            char szline[32];
            size_t sl = 0;
            while (sl + 1 < sizeof szline) {
                unsigned char c;
                int r = br_sslio_read(ioc, &c, 1);
                if (r <= 0) return -1;
                szline[sl++] = (char)c;
                if (sl >= 2 && szline[sl - 2] == '\r' && szline[sl - 1] == '\n') break;
            }
            szline[sl] = 0;
            long n = strtol(szline, NULL, 16);
            if (n < 0) return -1;
            if (n == 0) {
                /* trailing \r\n after final chunk */
                unsigned char tr[2];
                br_sslio_read(ioc, tr, 2);
                return 0;
            }
            long remaining = n;
            while (remaining > 0) {
                if (g_interrupt) return -2;
                size_t want = remaining > (long)sizeof buf ? sizeof buf : (size_t)remaining;
                int r = br_sslio_read(ioc, buf, want);
                if (r <= 0) return -1;
                if (cb((char *)buf, 1, (size_t)r, userdata) != (size_t)r) return -1;
                remaining -= r;
            }
            /* consume trailing \r\n */
            unsigned char tr[2];
            if (br_sslio_read(ioc, tr, 2) <= 0) return -1;
        }
    } else {
        long read_total = 0;
        for (;;) {
            if (g_interrupt) return -2;
            int r = br_sslio_read(ioc, buf, sizeof buf);
            if (r < 0) return -1;
            if (r == 0) break;
            if (cb((char *)buf, 1, (size_t)r, userdata) != (size_t)r) return -1;
            read_total += r;
            if (clen >= 0 && read_total >= clen) break;
        }
        return 0;
    }
}

/* POST body to url with x-api-key header; streams response body via on_chunk.
   Captures first 4 KB of error body into err_head (null-terminated).
   Returns 0 on HTTP 200, -1 on transport/HTTP error (sets *out_status if known),
   -2 on SIGINT. */
static int http_post_sse(const char *url, const char *api_key, const char *body,
                         stream_state_t *st, int *out_status,
                         char *err_head, size_t err_head_sz) {
    char host[256], path[1024];
    int port;
    if (parse_url(url, host, sizeof host, &port, path, sizeof path) < 0) return -1;
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);

    int fd = tcp_connect(host, portstr, 10);
    if (fd < 0) { fprintf(stderr, "\nconnect failed: %s\n", strerror(errno)); return -1; }

    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
    br_ssl_client_reset(&sc, host, 0);

    br_sslio_context ioc;
    br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);

    char req[4096];
    int rl = snprintf(req, sizeof req,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: tiny_c/0.1\r\n"
        "Content-Type: application/json\r\n"
        "Accept: text/event-stream\r\n"
        "Anthropic-Version: 2023-06-01\r\n"
        "X-API-Key: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, api_key, strlen(body));
    if (rl < 0 || (size_t)rl >= sizeof req) { close(fd); return -1; }

    if (br_sslio_write_all(&ioc, (const unsigned char *)req, rl) < 0) goto fail;
    if (br_sslio_write_all(&ioc, (const unsigned char *)body, strlen(body)) < 0) goto fail;
    if (br_sslio_flush(&ioc) < 0) goto fail;

    int status = 0, chunked = 0;
    long clen = -1;
    char hdr[8192];
    if (http_read_headers(&ioc, &status, hdr, sizeof hdr, &chunked, &clen) < 0) goto fail;
    if (out_status) *out_status = status;

    if (status != 200) {
        /* capture up to err_head_sz-1 bytes of body for the caller's raw_head */
        size_t used = 0;
        if (err_head && err_head_sz > 1) {
            unsigned char buf[512];
            while (used + 1 < err_head_sz) {
                int r = br_sslio_read(&ioc, buf, sizeof buf);
                if (r <= 0) break;
                size_t take = (size_t)r < err_head_sz - 1 - used ? (size_t)r : err_head_sz - 1 - used;
                memcpy(err_head + used, buf, take);
                used += take;
            }
            err_head[used] = 0;
        }
        br_sslio_close(&ioc);
        close(fd);
        return -1;
    }

    int r = http_stream_body(&ioc, chunked, clen, on_chunk, st);
    br_sslio_close(&ioc);
    close(fd);
    if (r == -2) return -2;
    if (r < 0) return -1;
    return 0;

fail:
    close(fd);
    return -1;
}

static void block_overflow_warn(int idx) {
    static int warned = 0;
    if (warned) return;
    warned = 1;
    fprintf(stderr, "\n[block index %d >= MAX_BLOCKS=%d; further blocks ignored]\n", idx, MAX_BLOCKS);
}

static void handle_event(stream_state_t *st, const char *json_str) {
    cJSON *evt = cJSON_Parse(json_str);
    if (!evt) return;
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(evt, "type"));
    if (!t) { cJSON_Delete(evt); return; }

    if (strcmp(t, "content_block_start") == 0) {
        cJSON *jidx = cJSON_GetObjectItem(evt, "index");
        int idx = cJSON_IsNumber(jidx) ? jidx->valueint : -1;
        if (idx >= MAX_BLOCKS) block_overflow_warn(idx);
        if (idx >= 0 && idx < MAX_BLOCKS) {
            cJSON *cb = cJSON_GetObjectItem(evt, "content_block");
            block_t *b = &st->blocks[idx];
            b->type[0] = b->id[0] = b->name[0] = 0;
            buf_reset(&b->text);
            buf_reset(&b->partial);
            const char *s;
            if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(cb, "type")))) { strncpy(b->type, s, sizeof b->type - 1); b->type[sizeof b->type - 1] = 0; }
            if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(cb, "id"))))   { strncpy(b->id, s, sizeof b->id - 1); b->id[sizeof b->id - 1] = 0; }
            if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(cb, "name")))) { strncpy(b->name, s, sizeof b->name - 1); b->name[sizeof b->name - 1] = 0; }
            if (idx + 1 > st->n) st->n = idx + 1;
        }
    } else if (strcmp(t, "content_block_delta") == 0) {
        cJSON *jidx = cJSON_GetObjectItem(evt, "index");
        int idx = cJSON_IsNumber(jidx) ? jidx->valueint : -1;
        if (idx >= MAX_BLOCKS) block_overflow_warn(idx);
        if (idx >= 0 && idx < MAX_BLOCKS) {
            cJSON *d = cJSON_GetObjectItem(evt, "delta");
            const char *dt = cJSON_GetStringValue(cJSON_GetObjectItem(d, "type"));
            if (!dt) { cJSON_Delete(evt); return; }
            block_t *b = &st->blocks[idx];
            if (strcmp(dt, "text_delta") == 0) {
                const char *txt = cJSON_GetStringValue(cJSON_GetObjectItem(d, "text"));
                if (txt && b->text.len < MAX_BLOCK_SIZE) {
                    size_t tl = strlen(txt);
                    size_t room = MAX_BLOCK_SIZE - b->text.len;
                    size_t take = tl < room ? tl : room;
                    buf_append(&b->text, txt, take);
                    fwrite(txt, 1, take, stdout);
                    fflush(stdout);
                    if (take < tl) fprintf(stderr, "\n[text block capped at %u bytes]\n", (unsigned)MAX_BLOCK_SIZE);
                }
            } else if (strcmp(dt, "input_json_delta") == 0) {
                const char *pj = cJSON_GetStringValue(cJSON_GetObjectItem(d, "partial_json"));
                if (pj && b->partial.len < MAX_BLOCK_SIZE) {
                    size_t pjl = strlen(pj);
                    size_t room = MAX_BLOCK_SIZE - b->partial.len;
                    size_t take = pjl < room ? pjl : room;
                    buf_append(&b->partial, pj, take);
                    if (take < pjl) fprintf(stderr, "\n[tool input capped at %u bytes]\n", (unsigned)MAX_BLOCK_SIZE);
                }
            }
        }
    } else if (strcmp(t, "error") == 0) {
        cJSON *err = cJSON_GetObjectItem(evt, "error");
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
        fprintf(stderr, "\n[api error: %s]\n", msg ? msg : "(no message)");
    }
    cJSON_Delete(evt);
}

static size_t on_chunk(char *ptr, size_t size, size_t nmemb, void *userdata) {
    if (g_interrupt) return 0;
    stream_state_t *st = userdata;
    size_t n = size * nmemb;
    if (g_debug) { fprintf(stderr, "\x1b[2m%.*s\x1b[0m", (int)n, ptr); fflush(stderr); }
    if (st->raw_head_len < sizeof st->raw_head) {
        size_t room = sizeof st->raw_head - st->raw_head_len;
        size_t take = n < room ? n : room;
        memcpy(st->raw_head + st->raw_head_len, ptr, take);
        st->raw_head_len += take;
    }
    for (size_t i = 0; i < n; i++) {
        char c = ptr[i];
        if (c == '\n') {
            if (st->line_dropped) {
                fprintf(stderr, "\n[dropped oversized SSE line]\n");
                st->line_dropped = 0;
            } else if (st->line.len > 5 && memcmp(st->line.data, "data:", 5) == 0) {
                const char *p = st->line.data + 5;
                if (*p == ' ') p++;
                handle_event(st, p);
            }
            buf_reset(&st->line);
        } else if (c != '\r') {
            if (st->line.len >= MAX_SSE_LINE) st->line_dropped = 1;
            else buf_append(&st->line, &c, 1);
        }
    }
    return n;
}

static cJSON *blocks_to_content(stream_state_t *st) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < st->n; i++) {
        block_t *b = &st->blocks[i];
        if (!b->type[0]) continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "type", b->type);
        if (strcmp(b->type, "text") == 0) {
            cJSON_AddStringToObject(obj, "text", b->text.data ? b->text.data : "");
        } else if (strcmp(b->type, "tool_use") == 0) {
            cJSON_AddStringToObject(obj, "id", b->id);
            cJSON_AddStringToObject(obj, "name", b->name);
            cJSON *input = b->partial.len > 0 ? cJSON_Parse(b->partial.data) : NULL;
            if (!input) input = cJSON_CreateObject();
            cJSON_AddItemToObject(obj, "input", input);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

static void stream_state_free(stream_state_t *st) {
    for (int i = 0; i < MAX_BLOCKS; i++) {
        buf_free(&st->blocks[i].text);
        buf_free(&st->blocks[i].partial);
    }
    buf_free(&st->line);
}

/* --- http --- */
static int slist_add(struct curl_slist **lst, const char *s) {
    struct curl_slist *n = curl_slist_append(*lst, s);
    if (!n) return -1;
    *lst = n;
    return 0;
}

static int do_turn(CURL *curl, const char *url, const char *api_key, const char *req_json, stream_state_t *st) {
    struct curl_slist *hdr = NULL;
    char auth[512];
    int na = snprintf(auth, sizeof auth, "x-api-key: %s", api_key);
    if (na < 0 || (size_t)na >= sizeof auth) { fprintf(stderr, "\napi key too long\n"); return -1; }
    if (slist_add(&hdr, "content-type: application/json") < 0
     || slist_add(&hdr, "anthropic-version: 2023-06-01") < 0
     || slist_add(&hdr, "accept: text/event-stream") < 0
     || slist_add(&hdr, auth) < 0) {
        curl_slist_free_all(hdr);
        fprintf(stderr, "\nslist alloc failed\n");
        return -1;
    }

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, req_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_chunk);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, st);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "tiny_c/0.1");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdr);

    if (g_interrupt) return -2;
    if (rc != CURLE_OK) { fprintf(stderr, "\ncurl: %s\n", curl_easy_strerror(rc)); return -1; }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) {
        fprintf(stderr, "\napi status %ld\n", status);
        int context_err = 0;
        if (st->raw_head_len > 0 && st->raw_head_len < sizeof st->raw_head) {
            st->raw_head[st->raw_head_len] = 0;
            cJSON *j = cJSON_Parse(st->raw_head);
            if (j) {
                cJSON *e = cJSON_GetObjectItem(j, "error");
                const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(e, "message"));
                if (m) {
                    fprintf(stderr, "error: %s\n", m);
                    if (strstr(m, "context") || strstr(m, "token") || strstr(m, "length")) context_err = 1;
                }
                cJSON_Delete(j);
            }
        }
        return context_err ? -3 : -1;
    }
    return 0;
}

/* --- tools --- */
static char *run_shell(const char *cmd) {
    buf_t out = {0};
    size_t wrapped_len = strlen(cmd) + 32;
    char *wrapped = malloc(wrapped_len);
    if (!wrapped) abort();
    snprintf(wrapped, wrapped_len, "%s 2>&1", cmd);
    FILE *f = popen(wrapped, "r");
    free(wrapped);
    if (!f) return strdup("error: popen failed");
    char chunk[4096];
    size_t got;
    int truncated = 0;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        buf_append(&out, chunk, got);
        if (out.len > (1u << 20)) { truncated = 1; break; }
    }
    int status = pclose(f);
    if (out.len == 0) buf_append(&out, "(no output)", 11);
    if (truncated) buf_append(&out, "\n[output truncated at 1MB]", 26);
    char trailer[64];
    if (WIFEXITED(status))        snprintf(trailer, sizeof trailer, "\n[exit %d]", WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) snprintf(trailer, sizeof trailer, "\n[signal %d]", WTERMSIG(status));
    else                          snprintf(trailer, sizeof trailer, "\n[status %d]", status);
    buf_append(&out, trailer, strlen(trailer));
    char *s = strdup(out.data);
    buf_free(&out);
    return s;
}

static char *read_file_tool(const char *path, long offset, long limit) {
    if (limit <= 0) limit = 2000;
    if (offset < 1) offset = 1;
    FILE *f = fopen(path, "r");
    if (!f) {
        char msg[512];
        snprintf(msg, sizeof msg, "error: cannot open %s: %s", path, strerror(errno));
        return strdup(msg);
    }
    buf_t out = {0};
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    long lineno = 0, lines_read = 0;
    size_t total = 0;
    const size_t MAX_BYTES = 256 * 1024;
    while ((len = getline(&line, &cap, f)) != -1) {
        if (memchr(line, 0, len)) {
            free(line); fclose(f); buf_free(&out);
            return strdup("error: binary file (NUL byte detected); use shell for binary reads");
        }
        lineno++;
        if (lineno < offset) continue;
        if (lines_read >= limit) break;
        char prefix[32];
        int plen = snprintf(prefix, sizeof prefix, "%6ld\t", lineno);
        buf_append(&out, prefix, plen);
        buf_append(&out, line, len);
        total += plen + len;
        lines_read++;
        if (total > MAX_BYTES) {
            buf_append(&out, "\n[truncated: 256KB cap reached]\n", 33);
            break;
        }
    }
    int hit_limit = (lines_read == limit);
    free(line);
    fclose(f);
    if (lines_read == 0) {
        buf_free(&out);
        if (lineno == 0) return strdup("(empty file)");
        char msg[128];
        snprintf(msg, sizeof msg, "error: offset %ld beyond end (file has %ld lines)", offset, lineno);
        return strdup(msg);
    }
    if (hit_limit) {
        char t[128];
        int tl = snprintf(t, sizeof t, "\n[limit %ld reached; use offset=%ld to continue]\n", limit, lineno);
        buf_append(&out, t, tl);
    }
    char *s = strdup(out.data);
    buf_free(&out);
    return s;
}

static char *atomic_write(const char *path, const char *content, size_t len) {
    char tmp[1024];
    int tn = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= sizeof tmp) return strdup("error: path too long");
    FILE *f = fopen(tmp, "w");
    if (!f) {
        char msg[512];
        snprintf(msg, sizeof msg, "error: open %s: %s", tmp, strerror(errno));
        return strdup(msg);
    }
    size_t wrote = fwrite(content, 1, len, f);
    int cerr = fclose(f);
    if (wrote != len || cerr != 0) { unlink(tmp); return strdup("error: write failed"); }
    if (rename(tmp, path) != 0) {
        char msg[512];
        snprintf(msg, sizeof msg, "error: rename: %s", strerror(errno));
        unlink(tmp);
        return strdup(msg);
    }
    char msg[128];
    snprintf(msg, sizeof msg, "wrote %zu bytes to %s", len, path);
    return strdup(msg);
}

static char *edit_file_tool(const char *path, const char *old_str, const char *new_str) {
    if (!old_str[0]) return atomic_write(path, new_str, strlen(new_str));

    FILE *f = fopen(path, "r");
    if (!f) {
        char msg[512];
        snprintf(msg, sizeof msg, "error: open %s: %s", path, strerror(errno));
        return strdup(msg);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (long)(4 * 1024 * 1024)) { fclose(f); return strdup("error: file too large for edit (>4MB)"); }
    char *body = malloc(sz + 1);
    if (!body) abort();
    size_t got = fread(body, 1, sz, f);
    fclose(f);
    body[got] = 0;

    size_t ol = strlen(old_str);
    const char *first = NULL;
    int count = 0;
    const char *p = body;
    while ((p = strstr(p, old_str))) {
        if (count == 0) first = p;
        count++;
        p += ol ? ol : 1;
    }
    if (count == 0) { free(body); return strdup("error: old_string not found"); }
    if (count > 1) {
        free(body);
        char msg[128];
        snprintf(msg, sizeof msg, "error: old_string matched %d times; make it unique", count);
        return strdup(msg);
    }

    size_t nl = strlen(new_str);
    size_t prefix = first - body;
    size_t new_sz = got - ol + nl;
    char *buf = malloc(new_sz + 1);
    if (!buf) abort();
    memcpy(buf, body, prefix);
    memcpy(buf + prefix, new_str, nl);
    memcpy(buf + prefix + nl, first + ol, got - prefix - ol);
    free(body);
    char *result = atomic_write(path, buf, new_sz);
    free(buf);
    return result;
}

/* --- skills --- */
typedef struct {
    char name[128];
    char description[512];
    char path[1024];
} skill_t;

static skill_t g_skills[MAX_SKILLS];
static int g_skill_count = 0;

static void trim(char *s) {
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = 0;
        memmove(s, s + 1, n - 1);
    }
}

static void append_fold(char *dst, size_t dst_size, const char *s, int fold) {
    size_t cur = strlen(dst);
    if (cur + 1 >= dst_size) return;
    if (cur > 0) {
        if (cur + 2 >= dst_size) return;
        dst[cur++] = fold ? ' ' : '\n';
        dst[cur] = 0;
    }
    strncat(dst, s, dst_size - strlen(dst) - 1);
}

static int parse_frontmatter(const char *path, skill_t *sk) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[2048];
    int in_fm = 0, saw_header = 0;
    char *cont_target = NULL;
    size_t cont_size = 0;
    int cont_fold = 1;
    sk->name[0] = 0;
    sk->description[0] = 0;
    while (fgets(line, sizeof line, f)) {
        int indented = (line[0] == ' ' || line[0] == '\t');
        char t[2048];
        strncpy(t, line, sizeof t - 1);
        t[sizeof t - 1] = 0;
        trim(t);
        if (!in_fm) {
            if (strcmp(t, "---") == 0) { in_fm = 1; saw_header = 1; continue; }
            if (t[0] != 0) break;
            continue;
        }
        if (strcmp(t, "---") == 0) break;
        if (cont_target && indented) {
            if (t[0] != 0) append_fold(cont_target, cont_size, t, cont_fold);
            continue;
        }
        cont_target = NULL;
        char *colon = strchr(t, ':');
        if (!colon) continue;
        *colon = 0;
        char *key = t;
        char *val = colon + 1;
        trim(key);
        trim(val);
        char *target = NULL;
        size_t tsize = 0;
        if (strcmp(key, "name") == 0) { target = sk->name; tsize = sizeof sk->name; }
        else if (strcmp(key, "description") == 0) { target = sk->description; tsize = sizeof sk->description; }
        else continue;
        target[0] = 0;
        if (val[0] == '>' && (val[1] == 0 || val[1] == '-' || val[1] == '+')) {
            cont_target = target; cont_size = tsize; cont_fold = 1;
        } else if (val[0] == '|' && (val[1] == 0 || val[1] == '-' || val[1] == '+')) {
            cont_target = target; cont_size = tsize; cont_fold = 0;
        } else {
            strncpy(target, val, tsize - 1);
            target[tsize - 1] = 0;
        }
    }
    fclose(f);
    return (saw_header && sk->name[0]) ? 0 : -1;
}

static void scan_skills_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (g_skill_count >= MAX_SKILLS) {
            static int warned = 0;
            if (!warned) { fprintf(stderr, "[skill cap %d reached; further skills ignored]\n", MAX_SKILLS); warned = 1; }
            break;
        }
        const char *n = ent->d_name;
        if (n[0] == '.') continue;
        char path[1024];
        int pn = snprintf(path, sizeof path, "%s/%s", dir, n);
        if (pn < 0 || (size_t)pn >= sizeof path) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        skill_t sk = {0};
        int ok = -1;
        const char *source_path = NULL;
        char skill_md[1024];
        if (S_ISREG(st.st_mode)) {
            size_t nl = strlen(n);
            if (nl < 4 || strcmp(n + nl - 3, ".md") != 0) continue;
            ok = parse_frontmatter(path, &sk);
            source_path = path;
        } else if (S_ISDIR(st.st_mode)) {
            int mn = snprintf(skill_md, sizeof skill_md, "%s/SKILL.md", path);
            if (mn < 0 || (size_t)mn >= sizeof skill_md) continue;
            ok = parse_frontmatter(skill_md, &sk);
            source_path = skill_md;
        }
        if (ok == 0) {
            char *resolved = realpath(source_path, NULL);
            const char *chosen = resolved ? resolved : source_path;
            strncpy(sk.path, chosen, sizeof sk.path - 1);
            sk.path[sizeof sk.path - 1] = 0;
            free(resolved);
            g_skills[g_skill_count++] = sk;
        }
    }
    closedir(d);
}

static int append_claude_md(buf_t *out, const char *path, const char *label) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    buf_append(out, "\n\nContents of ", 14);
    buf_append(out, path, strlen(path));
    buf_append(out, " (", 2);
    buf_append(out, label, strlen(label));
    buf_append(out, "):\n\n", 4);
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) buf_append(out, chunk, got);
    fclose(f);
    return 1;
}

static void buf_append_xml_escaped(buf_t *out, const char *s) {
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '<': buf_append(out, "&lt;", 4); break;
            case '>': buf_append(out, "&gt;", 4); break;
            case '&': buf_append(out, "&amp;", 5); break;
            default: buf_append(out, p, 1); break;
        }
    }
}

typedef struct {
    char path[1024];
    char paths[MAX_RULE_PATHS][256];
    int path_count;
    char *content;
} rule_t;

static rule_t g_rules[MAX_RULES];
static int g_rule_count = 0;

static int rule_applies(const rule_t *r) {
    if (r->path_count == 0) return 1;
    for (int i = 0; i < r->path_count; i++) {
        const char *g = r->paths[i];
        if (strncmp(g, "**/", 3) == 0) g += 3;
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "find . -type f -name '%s' -not -path '*/.git/*' -print -quit 2>/dev/null", g);
        FILE *fp = popen(cmd, "r"); if (!fp) continue;
        int found = fgetc(fp) != EOF; pclose(fp);
        if (found) return 1;
    }
    return 0;
}

static int parse_rule(const char *path, rule_t *r) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    r->content = malloc(sz + 1);
    if (!r->content) abort();
    r->content[fread(r->content, 1, sz, f)] = 0;
    fclose(f);
    strncpy(r->path, path, sizeof r->path - 1); r->path[sizeof r->path - 1] = 0;
    r->path_count = 0;
    if (strncmp(r->content, "---", 3)) return 0;
    char *p = strchr(r->content, '\n'); if (!p) return 0;
    int in_paths = 0;
    for (p++; *p; ) {
        char *eol = strchr(p, '\n'); size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= 3 && !strncmp(p, "---", 3)) break;
        if (p[0] != ' ' && p[0] != '\t') in_paths = (len >= 6 && !strncmp(p, "paths:", 6));
        else if (in_paths) {
            char *q = p; while (*q == ' ' || *q == '\t') q++;
            if (*q == '-') {
                q++; while (*q == ' ' || *q == '"' || *q == '\'') q++;
                char *e = eol ? eol : p + len;
                while (e > q && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '"' || e[-1] == '\'')) e--;
                size_t vl = e - q;
                if (vl && r->path_count < MAX_RULE_PATHS) {
                    size_t cl = vl < sizeof r->paths[0] - 1 ? vl : sizeof r->paths[0] - 1;
                    memcpy(r->paths[r->path_count], q, cl);
                    r->paths[r->path_count++][cl] = 0;
                }
            }
        }
        p = eol ? eol + 1 : p + len;
    }
    return 0;
}

static void scan_rules_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (g_rule_count >= MAX_RULES) break;
        const char *fn = ent->d_name;
        if (fn[0] == '.') continue;
        size_t fl = strlen(fn);
        if (fl < 4 || strcmp(fn + fl - 3, ".md") != 0) continue;
        char path[1024];
        int pn = snprintf(path, sizeof path, "%s/%s", dir, fn);
        if (pn < 0 || (size_t)pn >= sizeof path) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (st.st_size > 64 * 1024) { fprintf(stderr, "[rule >64KB skipped: %s]\n", path); continue; }
        if (parse_rule(path, &g_rules[g_rule_count]) == 0) g_rule_count++;
    }
    closedir(d);
}

static void build_system_prompt(buf_t *out) {
    buf_append(out, BASE_SYSTEM, strlen(BASE_SYSTEM));

    int loaded = 0;
    const char *home = getenv("HOME");
    if (home) {
        char p[1024];
        int pn = snprintf(p, sizeof p, "%s/.claude/CLAUDE.md", home);
        if (pn > 0 && (size_t)pn < sizeof p)
            loaded += append_claude_md(out, p, "user's private global instructions for all projects");
    }
    loaded += append_claude_md(out, "./CLAUDE.md", "project instructions");
    if (loaded > 0) fprintf(stderr, "[loaded %d CLAUDE.md]\n", loaded);

    for (int i = 0; i < g_rule_count; i++) {
        if (!rule_applies(&g_rules[i])) continue;
        buf_append(out, "\n\nContents of ", 14);
        buf_append(out, g_rules[i].path, strlen(g_rules[i].path));
        buf_append(out, " (rule):\n\n", 10);
        buf_append(out, g_rules[i].content, strlen(g_rules[i].content));
    }

    if (g_skill_count == 0) return;
    const char *hint = "\n\nTo use a skill, first read its SKILL.md (via `cat <path>`) to learn its instructions, then follow them. The path is a markdown file, not an executable.";
    buf_append(out, hint, strlen(hint));
    buf_append(out, "\n\n<skills>\n", 11);
    for (int i = 0; i < g_skill_count; i++) {
        buf_append(out, "  <skill>\n    <name>", 20);
        buf_append_xml_escaped(out, g_skills[i].name);
        buf_append(out, "</name>\n    <description>", 25);
        buf_append_xml_escaped(out, g_skills[i].description);
        buf_append(out, "</description>\n    <path>", 25);
        buf_append_xml_escaped(out, g_skills[i].path);
        buf_append(out, "</path>\n  </skill>\n", 19);
    }
    buf_append(out, "</skills>", 9);
}

/* --- session --- */
static void session_append(cJSON *msg) {
    FILE *f = fopen(SESSION_PATH, "a");
    if (!f) return;
    char *s = cJSON_PrintUnformatted(msg);
    if (s) { fputs(s, f); fputc('\n', f); free(s); }
    fclose(f);
}

static void session_rewrite(cJSON *messages) {
    FILE *f = fopen(SESSION_PATH ".tmp", "w");
    if (!f) return;
    int n = cJSON_GetArraySize(messages);
    for (int i = 0; i < n; i++) {
        char *s = cJSON_PrintUnformatted(cJSON_GetArrayItem(messages, i));
        if (s) { fputs(s, f); fputc('\n', f); free(s); }
    }
    fclose(f);
    rename(SESSION_PATH ".tmp", SESSION_PATH);
}

static int compact_clear_tool_results(cJSON *messages, int keep_last_n) {
    int n = cJSON_GetArraySize(messages);
    int keep_from = n > keep_last_n ? n - keep_last_n : 0;
    int cleared = 0;
    for (int i = 0; i < keep_from; i++) {
        cJSON *content = cJSON_GetObjectItem(cJSON_GetArrayItem(messages, i), "content");
        if (!cJSON_IsArray(content)) continue;
        int cn = cJSON_GetArraySize(content);
        for (int j = 0; j < cn; j++) {
            cJSON *item = cJSON_GetArrayItem(content, j);
            const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(item, "type"));
            if (!type || strcmp(type, "tool_result") != 0) continue;
            cJSON *c = cJSON_GetObjectItem(item, "content");
            if (!cJSON_IsString(c)) continue;
            const char *cur = c->valuestring;
            if (cur && strlen(cur) > 10 && strcmp(cur, "[cleared]") != 0) {
                cJSON_SetValuestring(c, "[cleared]");
                cleared++;
            }
        }
    }
    return cleared;
}

static int session_load(cJSON *messages) {
    FILE *f = fopen(SESSION_PATH, "r");
    if (!f) return -1;
    buf_t line = {0};
    int c;
    int count = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            if (line.len > 0) {
                cJSON *msg = cJSON_Parse(line.data);
                if (msg) { cJSON_AddItemToArray(messages, msg); count++; }
                else fprintf(stderr, "[session: malformed line skipped]\n");
            }
            buf_reset(&line);
        } else {
            char ch = (char)c;
            buf_append(&line, &ch, 1);
        }
    }
    if (line.len > 0) {
        cJSON *msg = cJSON_Parse(line.data);
        if (msg) { cJSON_AddItemToArray(messages, msg); count++; }
        else fprintf(stderr, "[session: malformed trailing line skipped]\n");
    }
    buf_free(&line);
    fclose(f);
    return count;
}

/* --- chat turn --- */
static int chat_turn(CURL *curl, const char *url, const char *api_key, const char *model,
                     const char *system_prompt, cJSON *messages, cJSON *tools) {
    for (int iter = 0; iter < MAX_TOOL_ITER; iter++) {
        if (g_interrupt) return 130;

        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "model", model);
        cJSON_AddNumberToObject(req, "max_tokens", 4096);
        cJSON_AddStringToObject(req, "system", system_prompt);
        cJSON_AddItemReferenceToObject(req, "messages", messages);
        cJSON_AddItemReferenceToObject(req, "tools", tools);
        cJSON_AddBoolToObject(req, "stream", cJSON_True);

        char *req_json = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);

        stream_state_t st = {0};
        int r = do_turn(curl, url, api_key, req_json, &st);
        free(req_json);
        fputc('\n', stdout);

        if (r == -3) {
            stream_state_free(&st);
            int cleared = compact_clear_tool_results(messages, 10);
            if (cleared > 0) {
                fprintf(stderr, "[compacted: cleared %d tool result%s; retrying]\n", cleared, cleared == 1 ? "" : "s");
                session_rewrite(messages);
                continue;
            }
            fprintf(stderr, "[context overflow with nothing to compact; bailing]\n");
            return 1;
        }
        if (r == -2) { fprintf(stderr, "\n[interrupted]\n"); stream_state_free(&st); return 130; }
        if (r < 0)   { stream_state_free(&st); return 1; }

        cJSON *assist = cJSON_CreateObject();
        cJSON_AddStringToObject(assist, "role", "assistant");
        cJSON_AddItemToObject(assist, "content", blocks_to_content(&st));
        cJSON_AddItemToArray(messages, assist);
        session_append(assist);

        int has_tool_use = 0;
        for (int i = 0; i < st.n; i++) if (strcmp(st.blocks[i].type, "tool_use") == 0) { has_tool_use = 1; break; }
        if (!has_tool_use) { stream_state_free(&st); return 0; }

        cJSON *results = cJSON_CreateArray();
        for (int i = 0; i < st.n; i++) {
            block_t *b = &st.blocks[i];
            if (strcmp(b->type, "tool_use") != 0) continue;

            cJSON *input = cJSON_Parse(b->partial.len > 0 ? b->partial.data : "{}");

            char *out;
            if (strcmp(b->name, "shell") == 0) {
                const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(input, "cmd"));
                if (!cmd) out = strdup("error: shell requires cmd");
                else {
                    fprintf(stderr, "\n\x1b[2m[shell] %s\x1b[0m\n", cmd);
                    out = run_shell(cmd);
                }
            } else if (strcmp(b->name, "edit_file") == 0) {
                const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(input, "path"));
                const char *os = cJSON_GetStringValue(cJSON_GetObjectItem(input, "old_string"));
                const char *ns = cJSON_GetStringValue(cJSON_GetObjectItem(input, "new_string"));
                if (!path || !os || !ns) out = strdup("error: edit_file requires path, old_string, new_string");
                else {
                    fprintf(stderr, "\n\x1b[2m[edit_file] %s (%s)\x1b[0m\n", path, os[0] ? "replace" : "write");
                    out = edit_file_tool(path, os, ns);
                }
            } else if (strcmp(b->name, "read_file") == 0) {
                const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(input, "path"));
                cJSON *joff = cJSON_GetObjectItem(input, "offset");
                cJSON *jlim = cJSON_GetObjectItem(input, "limit");
                long offset = cJSON_IsNumber(joff) ? (long)joff->valuedouble : 1;
                long limit  = cJSON_IsNumber(jlim) ? (long)jlim->valuedouble : 2000;
                if (!path) out = strdup("error: read_file requires path");
                else {
                    fprintf(stderr, "\n\x1b[2m[read_file] %s (offset=%ld limit=%ld)\x1b[0m\n", path, offset, limit);
                    out = read_file_tool(path, offset, limit);
                }
            } else {
                out = strdup("error: unknown tool");
            }
            cJSON_Delete(input);

            cJSON *res = cJSON_CreateObject();
            cJSON_AddStringToObject(res, "type", "tool_result");
            cJSON_AddStringToObject(res, "tool_use_id", b->id);
            cJSON_AddStringToObject(res, "content", out);
            cJSON_AddItemToArray(results, res);
            free(out);
        }
        cJSON *results_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(results_msg, "role", "user");
        cJSON_AddItemToObject(results_msg, "content", results);
        cJSON_AddItemToArray(messages, results_msg);
        session_append(results_msg);

        stream_state_free(&st);
    }
    fprintf(stderr, "\n[max tool iterations reached]\n");
    return 0;
}

/* --- main --- */
static cJSON *make_user_msg(const char *text) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(msg, "content", content);
    return msg;
}

static void add_prop(cJSON *props, const char *name, const char *type) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddItemToObject(props, name, p);
}

static void add_required(cJSON *schema, const char *name) {
    cJSON *a = cJSON_GetObjectItem(schema, "required");
    if (!a) { a = cJSON_CreateArray(); cJSON_AddItemToObject(schema, "required", a); }
    cJSON_AddItemToArray(a, cJSON_CreateString(name));
}

static cJSON *make_tool(const char *name, const char *desc) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", desc);
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
    cJSON_AddItemToObject(t, "input_schema", schema);
    return t;
}

static cJSON *tool_props(cJSON *tool) {
    return cJSON_GetObjectItem(cJSON_GetObjectItem(tool, "input_schema"), "properties");
}

static cJSON *tool_schema(cJSON *tool) {
    return cJSON_GetObjectItem(tool, "input_schema");
}

static cJSON *make_read_file_tool(void) {
    cJSON *t = make_tool("read_file", "Read a text file. Returns up to 'limit' lines starting from 'offset' (1-indexed), each prefixed with line number + tab. Default limit 2000. Hard cap 256KB. For larger files, page through with offset.");
    cJSON *props = tool_props(t);
    add_prop(props, "path", "string");
    add_prop(props, "offset", "integer");
    add_prop(props, "limit", "integer");
    add_required(tool_schema(t), "path");
    return t;
}

static cJSON *make_shell_tool(void) {
    cJSON *t = make_tool("shell", "Run a shell command via /bin/sh -c. Returns combined stdout+stderr and exit code.");
    add_prop(tool_props(t), "cmd", "string");
    add_required(tool_schema(t), "cmd");
    return t;
}

static cJSON *make_edit_file_tool(void) {
    cJSON *t = make_tool("edit_file", "Edit a file. If 'old_string' is empty, write 'new_string' as the full file contents (creates or overwrites). Otherwise, 'old_string' must appear exactly once in the file and will be replaced with 'new_string'. Errors on 0 or >1 matches. Atomic via .tmp + rename. File size cap 4 MB.");
    cJSON *props = tool_props(t);
    add_prop(props, "path", "string");
    add_prop(props, "old_string", "string");
    add_prop(props, "new_string", "string");
    add_required(tool_schema(t), "path");
    add_required(tool_schema(t), "old_string");
    add_required(tool_schema(t), "new_string");
    return t;
}

int main(int argc, char **argv) {
    int continue_flag = 0;
    int arg_start = 1;
    if (argc > 1 && (strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "--continue") == 0)) {
        continue_flag = 1;
        arg_start = 2;
    }

    const char *api_key = getenv("KIMI_TOKEN");
    if (!api_key) { fprintf(stderr, "set KIMI_TOKEN\n"); return 1; }
    if (getenv("TINY_DEBUG")) g_debug = 1;
    const char *base = getenv("KIMI_BASE_URL");
    if (!base) base = "https://api.kimi.com/coding";
    const char *model = getenv("MODEL");
    if (!model) model = "kimi-for-coding";

    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') base_len--;
    char url[1024];
    snprintf(url, sizeof url, "%.*s/v1/messages", (int)base_len, base);

    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();

    scan_skills_dir("./.claude/skills");
    scan_rules_dir("./.claude/rules");
    const char *home = getenv("HOME");
    if (home) {
        char hp[1024];
        int hn = snprintf(hp, sizeof hp, "%s/.claude/skills", home);
        if (hn > 0 && (size_t)hn < sizeof hp) scan_skills_dir(hp);
        hn = snprintf(hp, sizeof hp, "%s/.claude/rules", home);
        if (hn > 0 && (size_t)hn < sizeof hp) scan_rules_dir(hp);
    }
    if (g_skill_count > 0) fprintf(stderr, "[loaded %d skill%s]\n", g_skill_count, g_skill_count == 1 ? "" : "s");
    if (g_rule_count > 0) fprintf(stderr, "[loaded %d rule%s]\n", g_rule_count, g_rule_count == 1 ? "" : "s");

    buf_t sys = {0};
    build_system_prompt(&sys);
    const char *system_prompt = sys.data ? sys.data : BASE_SYSTEM;

    cJSON *messages = cJSON_CreateArray();
    if (continue_flag) {
        int loaded = session_load(messages);
        if (loaded < 0) fprintf(stderr, "[no prior session at " SESSION_PATH "]\n");
        else fprintf(stderr, "[resumed %d messages]\n", loaded);
    }

    cJSON *tools = cJSON_CreateArray();
    cJSON_AddItemToArray(tools, make_shell_tool());
    cJSON_AddItemToArray(tools, make_read_file_tool());
    cJSON_AddItemToArray(tools, make_edit_file_tool());

    int rc = 0;
    int oneshot = argc > arg_start;

    if (oneshot) {
        buf_t prompt = {0};
        for (int i = arg_start; i < argc; i++) {
            if (i > arg_start) buf_append(&prompt, " ", 1);
            buf_append(&prompt, argv[i], strlen(argv[i]));
        }
        cJSON *user_msg = make_user_msg(prompt.data ? prompt.data : "");
        cJSON_AddItemToArray(messages, user_msg);
        session_append(user_msg);
        buf_free(&prompt);
        rc = chat_turn(curl, url, api_key, model, system_prompt, messages, tools);
    } else {
        int is_tty = isatty(fileno(stdin));
        if (is_tty) fprintf(stderr, "tiny_c %s — Ctrl+D to exit, Ctrl+C to interrupt\n", model);
        char line[8192];
        for (;;) {
            if (is_tty) { fputs("\n\x1b[1;36m› \x1b[0m", stderr); fflush(stderr); }
            g_interrupt = 0;
            if (!fgets(line, sizeof line, stdin)) {
                if (g_interrupt) { g_interrupt = 0; clearerr(stdin); if (is_tty) fputc('\n', stderr); continue; }
                if (is_tty) fputc('\n', stderr);
                break;
            }
            size_t l = strlen(line);
            if (l > 0 && line[l - 1] == '\n') line[--l] = 0;
            if (l == 0) continue;
            cJSON *user_msg = make_user_msg(line);
            cJSON_AddItemToArray(messages, user_msg);
            session_append(user_msg);
            int r = chat_turn(curl, url, api_key, model, system_prompt, messages, tools);
            if (r == 1) { rc = 1; break; }
        }
    }

    cJSON_Delete(messages);
    cJSON_Delete(tools);
    buf_free(&sys);
    for (int i = 0; i < g_rule_count; i++) free(g_rules[i].content);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return rc;
}
