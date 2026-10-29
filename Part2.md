# Part 2 — Shared Memory Crisis (Threads + Mutex)

## Objective
Switch from processes to threads, and learn that the failure mode changes. Threads share the entire address space, so they share the buffer too. Now the corruption isn't only at the file offset — it's *inside the buffer itself*.

## Architecture
1. The **parent thread** (i.e. `main`) sets up redirection, parses argv, records start time.
2. The parent creates `num_workers` POSIX threads via `pthread_create`. Each thread runs a worker routine.
3. Each thread loops `writes_per_worker` times, formats a message via `snprintf` into a local buffer, and calls `my_write`.
4. The parent joins every thread with `pthread_join`.
5. The parent calls `flush_buffer()` once after all joins (to flush any tail bytes that didn't make it to a full chunk).
6. The parent records end time and reports to stderr.

## The Crisis (Read This Carefully)
With `fork()` (Part 1 / Part 3) each child gets its **own copy** of `buffer` and `buffer_pos`. The race is on the file offset only.

With `pthread_create` (this part) **all threads share the same `buffer` and `buffer_pos`**. This is much worse:
- Two threads may simultaneously increment `buffer_pos` and *both* think they own the slot, overwriting each other's bytes.
- One thread may be mid-copy when another flushes the buffer out from under it, sending half-formed data to the file *and* leaving the writer's loop pointing at garbage.
- One thread may flush, reset `buffer_pos` to `0`, and then a second thread's lingering write lands beyond the new `buffer_pos`, leaving holes filled with whatever was there before.

The fix is to make `my_write`'s body a **critical section**.

## Synchronization Requirement: One Mutex Around Buffer Manipulation
Use a single global `pthread_mutex_t` to protect everything that touches `buffer` or `buffer_pos`:

```c
pthread_mutex_t buf_mtx = PTHREAD_MUTEX_INITIALIZER;

static void my_write(const char *str) {
    pthread_mutex_lock(&buf_mtx);
    /* copy bytes; flush if buffer becomes full */
    pthread_mutex_unlock(&buf_mtx);
}
```

The lock must be **held for the duration of any access to `buffer`/`buffer_pos`**, including the flush call (`write(STDOUT_FILENO, buffer, CHUNK_SIZE)`) and the subsequent reset. You cannot drop the lock between "I see the buffer is full" and "I flush it" — that would re-introduce the race.

> **Conceptual question (the optional REPORT.md asks about this):** Could you make this lock *narrower* — for example, lock only the increment of `buffer_pos`, then drop the lock, then do the actual byte copy? Try it on paper. What goes wrong? (Hint: think about a context switch right after the increment.)

> **Note for Part 3.** In Part 3 the lock will look almost identical to this one — wrapping the whole `my_write` — but the **primitive** changes (named semaphore instead of mutex), and there is a subtle extra requirement that the buffer be *drained* before releasing the lock. The reason will become clear when you write it. Hold this thought.

## Cleanup Requirements
- All threads must be joined.
- The mutex must be destroyed with `pthread_mutex_destroy` before exit.
- The output fd must be closed.

## Build
```bash
gcc -Wall -Wextra -O2 -pthread -o part2 part2.c
```
The provided `Makefile` already does this.

## Optional: What to Put in REPORT.md for Part 2
*(REPORT.md is optional; filling it in earns up to +5 bonus points capped at 100. Skip this section if you don't plan to fill in the report.)*
1. **Output check.** Run `./part2 "Hello threaded world" 4 50 > /dev/null` and confirm `output.txt` is *clean* — every line matches the format. Paste 3 lines as evidence.
2. **Lock-scope question.** Answer the conceptual question above. Be specific about what shared state would race.
3. **CHUNK_SIZE experiment.** Run Part 2 with `./part2 "test" 50 1000` while temporarily changing `CHUNK_SIZE` to `8`, `64`, and `4096`. Report `elapsed_ms` for each. Explain in 2–3 sentences why the timing changes the way it does.

## Acceptance Criteria
- Compiles cleanly with `-Wall -Wextra -pthread` (no warnings).
- Output: every line matches the format regex; every `(ID, Seq)` pair appears **exactly once**; for each `ID` the `Seq` values are exactly `0..writes_per_worker-1`.
- No deadlocks (timeout is generous but enforced).
- Mutex is initialized and destroyed; threads are joined; fd is closed.

## Forbidden in Part 2
- `fork()` (this part is threads-only).
- More than one mutex (we only want the buffer mutex).
- `sem_*`.
- Any condition-variable trickery to "synchronize" the buffer differently. The point is: *one mutex, around the buffer-touching code*.
