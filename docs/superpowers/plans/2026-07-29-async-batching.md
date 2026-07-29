# Async Queue Batching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `mpsc_queue_get_batch()` and batch processing in `async_worker()` to minimize lock contention and I/O flush overhead.

**Architecture:**
- `include/queue.h` & `core/queue.c`: Implement `mpsc_queue_get_batch(queue, records, max_items)`.
- `core/async.c`: Update `async_worker()` to pop up to 32 records at once and flush once per batch.

---

### Task 1: Implement `mpsc_queue_get_batch()`

**Files:**
- Modify: `include/queue.h`
- Modify: `core/queue.c`
- Modify: `tests/test_queue_try_put.c`

- [ ] **Step 1: Add `mpsc_queue_get_batch` prototype in `include/queue.h`**

- [ ] **Step 2: Implement `mpsc_queue_get_batch` in `core/queue.c`**
  Acquire mutex once, pop up to `max_items` elements into buffer array, signal `not_full` if needed, unlock mutex once.

- [ ] **Step 3: Add unit test in `tests/test_queue_try_put.c`**

- [ ] **Step 4: Verify build & unit tests**
  Run: `make clean && make test`
  Expected: PASS

- [ ] **Step 5: Commit**
```bash
git add include/queue.h core/queue.c tests/test_queue_try_put.c
git commit -m "feat(queue): implement mpsc_queue_get_batch for batch dequeuing"
```

---

### Task 2: Integrate Batch Worker Processing in `core/async.c`

**Files:**
- Modify: `core/async.c`

- [ ] **Step 1: Update `async_worker()` in `core/async.c` to use batch popping**

- [ ] **Step 2: Verify build & async tests**
  Run: `make clean && make test`
  Expected: PASS

- [ ] **Step 3: Commit**
```bash
git add core/async.c
git commit -m "perf(async): use batch processing in async worker thread"
```

---

### Task 3: Quality Gate Verification

- [ ] **Step 1: Run standard test suite and format check**
  Run: `make check`
  Expected: ALL PASS

- [ ] **Step 2: Run ASan test suite**
  Run: `make test-asan`
  Expected: ALL PASS

- [ ] **Step 3: Run UBSan test suite**
  Run: `make test-ubsan`
  Expected: ALL PASS
