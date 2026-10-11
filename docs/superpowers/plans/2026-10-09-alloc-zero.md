# Hot-Path Zero-Alloc Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate all three verified per-record `malloc`s on the logging hot path (socket ring line copy, writer-thread newline framing, KV-path redact expansion) with zero throughput regression and unchanged public ABI.

**Architecture:** Slot-embedded 256 B inline storage with a heap-overflow flag threaded through the (internal-only) batch API; stack framing buffer with heap fallback in the writer thread; two-phase count-then-copy redact into caller-owned 512 B ping-pong buffers that outlive dispatch. Fail-open semantics preserved everywhere.

**Tech Stack:** C99 (`-std=c99 -Wall -Wextra -Wconversion -D_GNU_SOURCE -fvisibility=hidden`), Makefile + CMake (both kept in sync — though this plan adds no new files, so no registration changes needed).

---

## Corrections to the spec (read first)

Verification against the actual source overturned two spec items. Task 0 amends the spec file accordingly.

- `core/formatter.c` contains **zero** `malloc`/`calloc`/`strdup`/`realloc`/`free` (grep-confirmed). Spec §1 item 3 ("formatter 输出缓冲线程局部复用") is fiction — **dropped**.
- `core/socket_async.c` `malloc`s only at lines 74/81 (ring create, cold), 146 (ring put, **hot**), 655/701 (newline framing, **warm**), 726 (writer start, cold). There is **no** per-call TLS allocation — spec's "TLS format buf" item is fiction — **dropped**, replaced by the real framing mallocs.
- Text-path redact (`redact_apply_locked`, `core/log.c:861`) is already in-place on the caller buffer. Only the **KV path** (`redact_apply_one` → `malloc`, `core/log.c:952-998`) allocates. Scope locked to the KV path.

## File map

- Modify: `include/socket_async.h:76-81` (slot struct: add inline buf + heap flag), `:182-185` (`socket_ring_get_batch` gains `int *heaps` out-param). Internal header — free to change.
- Modify: `core/socket_async.c` — `socket_ring_put` (~:146), `socket_ring_get_batch` (~:177-262), `socket_ring_destroy` (~:308), `socket_writer_thread` (~:608-716, both batch loops + framing sites :655/:701).
- Modify: `core/log.c` — `redact_apply_one` (:952-998) split into count+copy, `redact_apply_all_locked` (:1003-1025) gains tmp buffers, `redact_kv_record_if_needed` (:919-947) threads them through, `logger_write_kv_internal` (:572-594) owns the 2×512 B buffers. All static except the internal writer — no public API change.
- Modify (tests only): `tests/test_socket_async.c` (new API + new tests), `tests/test_redact.c` (new behavior tests). No new test files → no Makefile/CMake registration churn.
- Modify: `docs/superpowers/specs/2026-10-09-alloc-zero-design.md` (Task 0 amendment), `CHANGELOG.md` (`[Unreleased]` → `Added`).

---

### Task 0: Amend the spec (drop fiction, lock framing scope)

**Files:**
- Modify: `docs/superpowers/specs/2026-10-09-alloc-zero-design.md`

- [ ] **Step 1: Replace §1 item 3 and the TLS item**

In §1, delete the "formatter 输出缓冲线程局部复用" item and the "TLS format buf" item, insert:

```markdown
- writer 线程换行 framing `malloc`（`core/socket_async.c:655,701`）：缺 `\n` 行逐条 `malloc(len+1)`，改为线程栈 `char framed_stack[512]` + 超长回退堆。
  （注：`core/formatter.c` 经 grep 确认零堆分配，原 formatter/TLS 两项取消。）
```

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-10-09-alloc-zero-design.md
git commit -m "docs(spec): 📝 correct alloc-zero spec (drop formatter/TLS, add framed fix)"
```
Expected: commit succeeds, working tree clean (`git status --short` empty).

---

### Task 1: Socket ring tests first (red — new API does not exist yet)

**Files:**
- Modify: `tests/test_socket_async.c`

- [ ] **Step 1: Migrate existing `socket_ring_get_batch` call sites to the new 5-arg form**

Run: `grep -n "socket_ring_get_batch" tests/test_socket_async.c`
Expected: lists every call site (known: the put/get round-trip test plus any others).

At each site, add an `int heaps[N];` array beside `lengths` and pass it; guard every `free((void *)lines[i])` with its flag. Known site (`test_ring_put_get`, ~line 45-59) becomes:

```c
    const char *lines[4];
    size_t      lengths[4];
    int         heaps[4];
    int         n = socket_ring_get_batch(ring, lines, lengths, heaps, 4);
    assert(n == 2);
    assert(lengths[0] == 5);
    assert(memcmp(lines[0], "line1", 5) == 0);
    assert(lengths[1] == 5);
    assert(memcmp(lines[1], "line2", 5) == 0);
    assert(clog_atomic_load_sz(&ring->count) == 0);

    for (int i = 0; i < n; i++) {
        if (heaps[i]) {
            free((void *)lines[i]);
        }
    }
```

- [ ] **Step 2: Add the three inline-storage tests (append beside the other ring tests)**

```c
static void test_ring_inline_short(void)
{
    socket_ring_buffer_t *ring = socket_ring_create(4);
    assert(ring != NULL);

    assert(socket_ring_put(ring, "hi", 2) == 0);

    const char *lines[4];
    size_t      lengths[4];
    int         heaps[4];
    int         n = socket_ring_get_batch(ring, lines, lengths, heaps, 4);
    assert(n == 1);
    assert(lengths[0] == 2);
    assert(memcmp(lines[0], "hi", 2) == 0);
    assert(heaps[0] == 0);
    assert(lines[0] == ring->slots[0].inline_buf); /* zero-alloc: points at slot storage */
    /* No free — inline storage dies with the ring. */

    socket_ring_destroy(ring);
    printf("  test_ring_inline_short PASSED\n");
}

static void test_ring_inline_boundary(void)
{
    char        buf255[255];
    char        buf256[256];
    const char *lines[4];
    size_t      lengths[4];
    int         heaps[4];
    socket_ring_buffer_t *ring = socket_ring_create(4);

    memset(buf255, 'a', sizeof(buf255));
    memset(buf256, 'b', sizeof(buf256));
    assert(ring != NULL);
    assert(socket_ring_put(ring, buf255, sizeof(buf255)) == 0); /* 255 + NUL = 256: inline */
    assert(socket_ring_put(ring, buf256, sizeof(buf256)) == 0); /* 256 + NUL = 257: heap  */
    assert(socket_ring_get_batch(ring, lines, lengths, heaps, 4) == 2);
    assert(heaps[0] == 0);
    assert(lengths[0] == sizeof(buf255));
    assert(memcmp(lines[0], buf255, sizeof(buf255)) == 0);
    assert(heaps[1] == 1);
    assert(lengths[1] == sizeof(buf256));
    assert(memcmp(lines[1], buf256, sizeof(buf256)) == 0);
    free((void *)lines[1]);

    socket_ring_destroy(ring);
    printf("  test_ring_inline_boundary PASSED\n");
}

static void test_ring_inline_destroy_pending(void)
{
    /* Inline lines pending in the ring at destroy time must not be freed. */
    socket_ring_buffer_t *ring = socket_ring_create(4);
    assert(ring != NULL);
    assert(socket_ring_put(ring, "pending", 7) == 0);
    socket_ring_destroy(ring); /* ASan gate catches any free-of-inline-storage */
    printf("  test_ring_inline_destroy_pending PASSED\n");
}
```

Register the three calls in `main()` next to the existing ring tests:

```c
    test_ring_inline_short();
    test_ring_inline_boundary();
    test_ring_inline_destroy_pending();
```

- [ ] **Step 3: Run and confirm it fails (API does not exist yet)**

Run: `make build/test_socket_async`
Expected: COMPILE FAIL — `socket_ring_slot_t has no member named 'heap'` / `too many arguments to socket_ring_get_batch`. This is the correct TDD red.

---

### Task 2: Socket ring implementation (green)

**Files:**
- Modify: `include/socket_async.h`
- Modify: `core/socket_async.c`

- [ ] **Step 1: Extend the slot struct (`include/socket_async.h:76-81`)**

```c
/** Maximum line length served from slot-embedded storage (incl. NUL). */
#define SOCKET_RING_INLINE_LEN 256

typedef struct {
    char             inline_buf[SOCKET_RING_INLINE_LEN]; /**< Zero-alloc fast-path storage. */
    char            *line; /**< Points to inline_buf (heap == 0) or malloc'd memory (heap == 1). */
    size_t            len;  /**< Length of @ref line (excluding NUL). */
    int              heap; /**< 1 if @ref line is heap-owned and the consumer must free it. */
    volatile uint64_t seq;  /**< Publish counter — release-stored by producer,
                                  acquire-loaded by consumer. */
} socket_ring_slot_t;
```

Update the `socket_ring_get_batch` doc + signature (`:182-185`):

```c
 * @param heaps      Output array of ownership flags, parallel to @p lines:
 *                   1 if the line is heap-allocated (caller must free),
 *                   0 if it points at slot-embedded storage (must NOT free).
...
int socket_ring_get_batch(socket_ring_buffer_t *ring,
                           const char          **lines,
                           size_t               *lengths,
                           int                  *heaps,
                           size_t                max_lines);
```

- [ ] **Step 2: `socket_ring_put` — inline fast path (`core/socket_async.c:145-166`)**

Replace the unconditional `malloc` block with:

```c
            socket_ring_slot_t *slot = &ring->slots[head & ring->mask];
            if (len < SOCKET_RING_INLINE_LEN) {
                memcpy(slot->inline_buf, line, len);
                slot->inline_buf[len] = '\0';
                slot->line            = slot->inline_buf;
                slot->len             = len;
                slot->heap            = 0;
            } else {
                char *copy = malloc(len + 1);
                if (!copy) {
                    /*
                     * Allocation failed. The slot is already claimed — rolling
                     * head back would corrupt other producers' claims, so publish
                     * an empty slot instead and count it as dropped. The consumer
                     * frees NULL safely and skips zero-length lines.
                     */
                    slot->line = NULL;
                    slot->len  = 0;
                    slot->heap = 0;
                    clog_atomic_store_u64(&slot->seq, (uint64_t)head + 1);
                    clog_atomic_fetch_add_sz(&ring->count, 1);
                    clog_sem_post(&ring->items_sem);
                    clog_atomic_inc64(&ring->dropped);
                    return -1;
                }
                memcpy(copy, line, len);
                copy[len] = '\0';

                slot->line = copy;
                slot->len  = len;
                slot->heap = 1;
            }
```

Keep the publish sequence (`seq` store, `count` inc, `sem_post`, `return 0`) exactly as-is below this block.

- [ ] **Step 3: `socket_ring_get_batch` + `socket_ring_destroy` — thread the flag**

In `socket_ring_get_batch`: null-check `heaps` alongside the other params (`if (!ring || !lines || !lengths || !heaps || max_lines == 0)`), and in the dequeue loop add:

```c
            lines[n]   = slot->line;
            lengths[n] = slot->len;
            heaps[n]   = slot->heap;
            slot->line = NULL;
            slot->len  = 0;
            slot->heap = 0;
```

In `socket_ring_destroy` (~:308) change the drain loop to:

```c
        free(ring->slots[idx].line);
```
→
```c
        if (ring->slots[idx].heap) {
            free(ring->slots[idx].line);
        }
```
This guard is load-bearing: freeing `inline_buf` would crash. The `test_ring_inline_destroy_pending` test + ASan verify it.

- [ ] **Step 4: `socket_writer_thread` — flag-guarded frees + stack framing**

Add beside `lines`/`lengths` (~:613-615):

```c
    const size_t BATCH_SIZE = 64;
    const char  *lines[64];
    size_t       lengths[64];
    int          heap_flags[64];
    char         framed_stack[512];
```

Pass `heap_flags` at both `socket_ring_get_batch` calls (~:619, ~:689). Re-grep `free((void *)lines` in the function and guard **every** site (`~:637,648,663,669,695,709` — confirm with grep, lines shift after edits):

```c
free((void *)lines[i]);
```
→
```c
if (heap_flags[i]) {
    free((void *)lines[i]);
}
```

Replace both framing mallocs (~:655, ~:701):

```c
                char *framed = malloc(lengths[i] + 1);
                if (framed) {
                    memcpy(framed, lines[i], lengths[i]);
                    framed[lengths[i]] = '\n';
                    rc                 = socket_writer_send(writer, framed, lengths[i] + 1);
                    free(framed);
                }
```
→
```c
                size_t framed_len = lengths[i] + 1;
                char  *framed =
                    (framed_len <= sizeof(framed_stack)) ? framed_stack : malloc(framed_len);
                if (framed) {
                    memcpy(framed, lines[i], lengths[i]);
                    framed[lengths[i]] = '\n';
                    rc                 = socket_writer_send(writer, framed, framed_len);
                    if (framed != framed_stack) {
                        free(framed);
                    }
                }
```

(Second site uses `socket_writer_send(writer, framed, lengths[i] + 1);` without `rc` — same replacement minus the `rc =`.)

- [ ] **Step 5: Build, run, ASan, commit**

```bash
make build/test_socket_async && ./build/test_socket_async
```
Expected: all tests incl. the 3 new ones PASS.

```bash
make build/test_socket_async CONFIG=asan && ./build/test_socket_async
```
Expected: PASS with no ASan errors (proves no free-of-inline-storage, no leaks on the heap path).

```bash
git add include/socket_async.h core/socket_async.c tests/test_socket_async.c
git commit -m "perf(socket): ⚡️ zero-alloc ring slots (256B inline) + stack newline framing"
```
Expected: commit succeeds.

---

### Task 3: KV redact tests (behavior lock — green before refactor)

**Files:**
- Modify: `tests/test_redact.c`

- [ ] **Step 1: Add KV redact behavior tests (append near the other tests)**

`CLOG_KV_STR` (see `tests/test_kv_logging.c:85`) and the file-logger helpers (`make_json_file_logger`, `read_whole`) already exist:

```c
static void test_kv_redact_short_str(void)
{
    logger_t *logger;
    char      out[4096];

    clog_redact_clear();
    assert(clog_redact_add("secret123", NULL) == CLOG_OK);

    logger = make_json_file_logger("logs/test_redact_kv_short.log");
    assert(logger != NULL);
    LOGGER_INFO_KV(logger, "user login", CLOG_KV_STR("token", "abc secret123 xyz"));
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_kv_short.log", out, sizeof(out));
    assert(strstr(out, "secret123") == NULL);
    assert(strstr(out, "***") != NULL);
    clog_redact_clear();
    printf("test_kv_redact_short_str passed\n");
}

static void test_kv_redact_multi_rule(void)
{
    logger_t *logger;
    char      out[4096];
    char      msg[256];

    clog_redact_clear();
    assert(clog_redact_add("alpha", "[A]") == CLOG_OK);
    assert(clog_redact_add("beta", "[B]") == CLOG_OK);

    snprintf(msg, sizeof(msg), "alpha and beta");
    logger = make_json_file_logger("logs/test_redact_kv_multi.log");
    assert(logger != NULL);
    LOGGER_INFO_KV(logger, msg, CLOG_KV_STR("k", "v"));
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_kv_multi.log", out, sizeof(out));
    assert(strstr(out, "alpha") == NULL);
    assert(strstr(out, "beta") == NULL);
    assert(strstr(out, "[A]") != NULL);
    assert(strstr(out, "[B]") != NULL);
    clog_redact_clear();
    printf("test_kv_redact_multi_rule passed\n");
}

static void test_kv_redact_long_value(void)
{
    /* 600B value forces the heap-fallback path — output must stay exact. */
    static char big[640];
    logger_t   *logger;
    char        out[4096];

    memset(big, 'x', 600);
    memcpy(big + 600, "secret123", 9);
    big[609] = '\0';

    clog_redact_clear();
    assert(clog_redact_add("secret123", NULL) == CLOG_OK);

    logger = make_json_file_logger("logs/test_redact_kv_long.log");
    assert(logger != NULL);
    LOGGER_INFO_KV(logger, "big value", CLOG_KV_STR("blob", big));
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_kv_long.log", out, sizeof(out));
    assert(strstr(out, "secret123") == NULL);
    assert(strstr(out, "***") != NULL);
    clog_redact_clear();
    printf("test_kv_redact_long_value passed\n");
}
```

Register in `main()`:

```c
    test_kv_redact_short_str();
    test_kv_redact_multi_rule();
    test_kv_redact_long_value();
```

- [ ] **Step 2: Run — must be GREEN before the refactor (locks behavior)**

Run: `make build/test_redact && ./build/test_redact`
Expected: PASS (refactor must preserve exactly this).

---

### Task 4: KV redact implementation (stack fast path, exact semantics)

**Files:**
- Modify: `core/log.c`

Design (locked): caller-owned 512 B ping-pong buffers live in `logger_write_kv_internal`'s frame, covering dispatch + free. Only `record->message` uses them (the hot field); STR KV values keep the existing heap chain (YAGNI — unchanged behavior). Multi-rule chains alternate buffers; any overflow rule falls back to `malloc`; final heap (if any) returns through `*p_owned_msg` so `free(owned_msg)` at `:590` works unchanged (`free(NULL)` is a no-op on the stack path). Fail-open (allocation failure → keep prior pointer) preserved.

- [ ] **Step 1: Split `redact_apply_one` into count + dest-write (`core/log.c:952-998`)**

Replace it with:

```c
/* Count occurrences of @p pat in @p src and compute the expanded length.
 * Returns the length of the replaced string (excluding NUL); *p_count is
 * the hit count (0 when the pattern does not occur). */
static size_t redact_expand_len(const char *src,
                                const char *pat,
                                size_t      pat_len,
                                size_t      msk_len,
                                size_t     *p_count)
{
    size_t      count = 0;
    size_t      src_len;
    const char *cur = src;
    const char *hit;

    while ((hit = strstr(cur, pat)) != NULL) {
        count++;
        cur = hit + pat_len;
    }
    *p_count = count;
    if (count == 0) {
        return strlen(src);
    }
    src_len = strlen(src);
    return (src_len - count * pat_len) + count * msk_len;
}

/* Copy @p src to @p dst replacing every @p pat with @p msk.
 * @p dst must hold redact_expand_len(...)+1 bytes. */
static void redact_expand_copy(const char *src,
                               const char *pat,
                               size_t      pat_len,
                               const char *msk,
                               size_t      msk_len,
                               char       *dst)
{
    const char *cur = src;
    const char *hit;

    while ((hit = strstr(cur, pat)) != NULL) {
        size_t head = (size_t)(hit - cur);
        size_t k;

        memcpy(dst, cur, head);
        dst += head;
        for (k = 0; k < msk_len; k++) {
            dst[k] = msk[k];
        }
        dst += msk_len;
        cur = hit + pat_len;
    }
    {
        size_t rest = strlen(cur) + 1;

        memcpy(dst, cur, rest);
    }
}

/* Single-rule replace-all into caller-chosen storage. Uses @p tmp (of
 * @p tmp_size bytes) when the result fits, malloc otherwise. Returns NULL
 * when the pattern does not occur or on allocation failure (caller then
 * keeps the original pointer: fail-open, never fail-closed).
 * *p_on_stack is 1 when the result lives in @p tmp. */
static char *redact_apply_one_buf(const char *src,
                                  const char *pat,
                                  const char *msk,
                                  char       *tmp,
                                  size_t      tmp_size,
                                  int        *p_on_stack)
{
    size_t pat_len = strlen(pat);
    size_t msk_len = strlen(msk);
    size_t count   = 0;
    size_t new_len = redact_expand_len(src, pat, pat_len, msk_len, &count);
    char  *dst;

    if (count == 0) {
        return NULL;
    }
    if (new_len + 1 <= tmp_size) {
        dst         = tmp;
        *p_on_stack = 1;
    } else {
        dst = (char *)malloc(new_len + 1);
        if (dst == NULL) {
            return NULL;
        }
        *p_on_stack = 0;
    }
    redact_expand_copy(src, pat, pat_len, msk, msk_len, dst);
    return dst;
}
```

Keep the old `redact_apply_one` name working for the STR-value path? No — STR path also routes through `redact_apply_all_locked`; see Step 2. Delete `redact_apply_one` outright (verify no other callers: `grep -n "redact_apply_one" core/log.c` must show only the deleted def + the new call below).

- [ ] **Step 2: Chain through ping-pong buffers (`core/log.c:1000-1025`)**

```c
/** Stack scratch size for the KV message redact fast path (see below). */
#define REDACT_KV_TMP_LEN 512

/* Requires g_redact_mutex held. Applies all rules in order to @p src.
 * Rule outputs go to @p tmp_a / @p tmp_b (alternating) when they fit,
 * malloc otherwise. Returns the final string (== @p src when nothing
 * matched); *p_owned is the live heap block or NULL when the result is
 * @p src or stack-owned (caller frees *p_owned with free() — free(NULL)
 * is a no-op, so the stack path needs no special handling). */
static const char *redact_apply_all_locked(const char *src, char *tmp_a, char *tmp_b, char **p_owned)
{
    const char *cur     = src;
    char       *owned   = NULL;
    char       *tmps[2] = {tmp_a, tmp_b};
    int         alt     = 0;
    size_t      r;

    for (r = 0; r < g_redact_count; r++) {
        const char *pat = g_redact_rules[r].pattern;
        const char *msk = g_redact_rules[r].mask;
        int         on_stack;
        char       *rep;

        if (pat[0] == '\0' || msk[0] == '\0') {
            continue;
        }
        rep = redact_apply_one_buf(cur, pat, msk, tmps[alt], REDACT_KV_TMP_LEN, &on_stack);
        if (rep != NULL) {
            if (owned != NULL && cur == owned) {
                free(owned); /* superseded intermediate */
                owned = NULL;
            }
            if (on_stack) {
                alt ^= 1; /* next rule reads this buffer, writes the other */
            } else {
                owned = rep;
            }
            cur = rep;
        }
    }
    *p_owned = owned;
    return cur;
}
```

Update `redact_kv_record_if_needed` (`:919-947`) to take and forward the buffers for the **message** only; STR values keep the old heap behavior via `malloc`-forced calls. Simplest exact-preserving split: give the helper a flag. Change its signature to:

```c
static void redact_kv_record_if_needed(log_record_t *record,
                                       char         **p_owned_msg,
                                       char          *owned_str[],
                                       char          *tmp_a,
                                       char          *tmp_b)
```

Message path:

```c
    clog_mutex_lock(&g_redact_mutex);
    if (g_redact_count > 0) {
        char       *msg_owned = NULL;
        const char *msg_final = redact_apply_all_locked(record->message, tmp_a, tmp_b, &msg_owned);

        owned_msg = msg_owned;
        if (msg_final != record->message) {
            record->message = (char *)msg_final;
        }
        for (vi = 0; vi < record->kv_count; vi++) {
            if (record->kv[vi].type == CLOG_KV_TYPE_STR && record->kv[vi].val.str != NULL) {
                /* STR values keep the heap path: pass an unusable tmp so the
                 * result is always malloc'd (behavior identical to before). */
                int         dummy = 0;
                char        tiny[1];
                char       *rep = redact_apply_one_buf(record->kv[vi].val.str,
                                                       g_redact_rules[0].pattern /* placeholder */,
                                                       "", tiny, sizeof(tiny), &dummy);
```

No — that only applies one rule. STR path needs the all-rules chain with forced heap. Cleaner: `redact_apply_all_locked` takes `char *tmp_a` which may be NULL meaning "heap only":

In `redact_apply_all_locked`, dest choice becomes:

```c
        rep = (tmp_a != NULL)
                  ? redact_apply_one_buf(cur, pat, msk, tmps[alt], REDACT_KV_TMP_LEN, &on_stack)
                  : redact_apply_one_buf(cur, pat, msk, tiny_heap_only...);
```

Ugly. Better: keep **two** functions — the new tmp-chain for the message, and rename the *original* `redact_apply_all_locked` body (heap-only) to `redact_apply_all_heap_locked` for STR values, implemented on top of `redact_apply_one_buf` with a 1-byte tmp:

```c
/* Heap-only variant for STR KV values (unchanged behavior, one malloc per
 * matching rule as before). */
static char *redact_apply_all_heap_locked(const char *src)
{
    const char *cur   = src;
    char       *owned = NULL;
    char        tiny[1];
    size_t      r;

    for (r = 0; r < g_redact_count; r++) {
        const char *pat = g_redact_rules[r].pattern;
        const char *msk = g_redact_rules[r].mask;
        int         on_stack;
        char       *rep;

        if (pat[0] == '\0' || msk[0] == '\0') {
            continue;
        }
        /* 1-byte tmp never fits a real replacement (NUL alone needs 1 and
         * count > 0 implies new_len >= 1... hmm, new_len+1 <= 1 means empty —
         * count > 0 guarantees new_len + 1 >= 2, so heap always). */
        rep = redact_apply_one_buf(cur, pat, msk, tiny, sizeof(tiny), &on_stack);
        if (rep != NULL) {
            free(owned);
            owned = rep;
            cur   = owned;
        }
    }
    return owned;
}
```

Wait — `free(owned)` when owned==NULL first time: fine. But if rep==cur? rep is never cur (tmp or fresh malloc) — matches original `free(owned); owned = rep;` semantics exactly. And `on_stack` is always 0 here (prove: count>0 → new_len ≥ ... new string nonempty → new_len+1 ≥ 2 > 1 ✓). Add `assert(on_stack == 0)`? diag: `(void)on_stack;` to avoid -Wunused... it IS used (passed by ptr). Add assert for documentation: `assert(rep == NULL || on_stack == 0);` — needs <assert.h>, check log.c includes it. If unsure, skip assert, keep comment.

Hmm — `char tiny[1]` + `redact_apply_one_buf` writes to tmp only when fits; never fits → never touched ✓.

STR loop in `redact_kv_record_if_needed` keeps its shape, calling `redact_apply_all_heap_locked` instead of `redact_apply_all_locked`:

```c
        for (vi = 0; vi < record->kv_count; vi++) {
            if (record->kv[vi].type == CLOG_KV_TYPE_STR && record->kv[vi].val.str != NULL) {
                char *rep = redact_apply_all_heap_locked(record->kv[vi].val.str);

                if (rep != NULL) {
                    record->kv[vi].val.str = rep;
                    owned_str[vi]           = rep;
                }
            }
        }
```

(Check the existing tail of the function `:934-947` — preserve its `*p_owned_msg = owned_msg;` assignment and structure; only swap the called functions.)

- [ ] **Step 3: Own the buffers in `logger_write_kv_internal` (`core/log.c:572-576`)**

```c
    char  *owned_msg              = NULL;
    char  *owned_str[CLOG_MAX_KV] = {NULL};
    char   redact_tmp_a[REDACT_KV_TMP_LEN];
    char   redact_tmp_b[REDACT_KV_TMP_LEN];
    size_t vi;

    redact_kv_record_if_needed(&record, &owned_msg, owned_str, redact_tmp_a, redact_tmp_b);
```

Lifetime covers dispatch (`:578-589`) and `free(owned_msg)` (`:590`, no-op when NULL) ✓. Async path clones synchronously inside `log_async_write_for` (`:579`) while buffers are live ✓.

- [ ] **Step 4: Build, run, ASan, commit**

```bash
make build/test_redact && ./build/test_redact
```
Expected: PASS — same output as Task 3 Step 2 (behavior preserved).

```bash
make build/test_redact CONFIG=asan && ./build/test_redact
```
Expected: PASS, no ASan errors (no stack-use-after-scope: buffers outlive dispatch).

```bash
make build/test_kv_logging && ./build/test_kv_logging
```
Expected: PASS (KV paths without redact unaffected).

```bash
git add core/log.c tests/test_redact.c
git commit -m "perf(redact): ⚡️ zero-alloc KV message redact (512B caller stack buffers)"
```
Expected: commit succeeds.

---

### Task 5: Full gate + CHANGELOG

**Files:**
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Run the full test suite**

Run: `make test`
Expected: every test binary passes (run from repo root; tests assume cwd = repo root).

- [ ] **Step 2: Run the pre-PR gate**

Run: `make check`
Expected: format check, clang-tidy (warnings-as-errors), unused-includes check, clean rebuild, all tests — all green. New code must be warning-free under `-Wconversion`: loop indices are `size_t`, flags are `int` assigned only `0`/`1`/comparison results, `lengths[i] + 1` stays `size_t`.

- [ ] **Step 3: CHANGELOG entry + commit**

Under `## [Unreleased]` → `### Added`, append:

```markdown
- Hot-path zero-alloc: socket ring slots carry 256 B inline storage (`SOCKET_RING_INLINE_LEN`, heap overflow flag via internal `socket_ring_get_batch` batch API — public ABI unchanged); writer-thread newline framing uses a 512 B stack buffer with heap fallback; KV message redact expands into caller-owned 512 B ping-pong stack buffers (STR values keep the heap path). Fail-open redact semantics preserved.
```

```bash
git add CHANGELOG.md
git commit -m "docs: 📝 changelog for hot-path zero-alloc work"
```
Expected: commit succeeds.

---

## Self-review (ran by plan author, not a subagent)

1. **Spec coverage:** socket inline (§1.1 ✓ Task 1-2), writer framing (amended §1 ✓ Task 2 Step 4), KV redact stack buf (§1.2/§2 ✓ Task 4). Formatter + TLS items removed with justification in Task 0. ABI-stable constraint ✓ (only `socket_async.h` internal header + static functions touched; `socket_ring_put` signature unchanged so producer callers in `sinks/` are unaffected). Perf constraint ✓ (same algorithmic work, fewer syscalls-free mallocs; `make benchmark` comparison available as follow-up, not gated).
2. **Placeholder scan:** no TBD/TODO/"similar to"; every code step ships complete bodies; grep-based call-site discovery steps name exact commands and the known sites.
3. **Type consistency:** `SOCKET_RING_INLINE_LEN` (256) used in header + put condition `len < SOCKET_RING_INLINE_LEN` (255+NUL fits, 256+NUL overflows — locked by boundary test); `heaps`/`heap_flags` are `int` end-to-end; `REDACT_KV_TMP_LEN` (512) shared by macro + long-value test (600 B > 512 forces fallback ✓); `framed_len` is `size_t` ✓.
