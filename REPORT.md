# REPORT — Exercise 2 (Optional Bonus)

> Filling this out is **optional**. You can earn up to **+5 bonus points** on top of your code score (capped at 100 total).

**Name:**
**Student ID (5 digits):**
**Date:**

---

## 1. Buffer Logic

In your own words, describe how your `my_write` handles a string `str` that is **longer** than the remaining space in `buffer`. What happens when `str` is even longer than `CHUNK_SIZE` itself?

> *Your answer here.*

---

## 2. Part 1 — Observed Chaos

Run:
```
./part1 "Hello concurrent world" 4 50
```

### 2a. Excerpt
Paste 5–10 consecutive lines from `output.txt` that show interleaved or corrupted output (you may need a heavier load — e.g. `8 2000` — to see clear corruption depending on how many cores your machine has):

```
<paste here>
```

### 2b. Explanation
Explain *why* the corruption looks the way it does. Specifically address: where does `buffer` live in memory after `fork()`? Where does the file offset live? Which one is shared, and which one is duplicated?

> *Your answer here.*

---

## 3. Part 2 — Mutex Scope

### 3a. Could you make the lock narrower?
You're holding the mutex for the entire duration of `my_write`. Could you instead lock only the increment of `buffer_pos`, drop the lock, and then do the byte copy outside the lock? Explain — concretely — what would go wrong.

> *Your answer here.*

### 3b. CHUNK_SIZE Experiment
Run `./part2 "test" 50 1000` with three different values of `CHUNK_SIZE` (recompile each time):

| CHUNK_SIZE | elapsed_ms |
|---|---|
| 8    |  |
| 64   |  |
| 4096 |  |

Explain why timing changes the way it does. (Hint: think about the number of `write()` syscalls vs. the contention on the mutex.)

> *Your answer here.*

---

## 4. Part 3 — The Centerpiece Question

In Part 2 you locked the entire `my_write` with a mutex. In Part 3 you also lock the entire `my_write` — but with a named semaphore, **and** you must drain the buffer before releasing the lock.

### 4a. Why does the lock have to wrap the whole `my_write`?
Suppose you only locked the `write()` syscall in Part 3. Construct a concrete byte-level interleaving between two processes that produces a corrupted output line. Show what would end up in `output.txt`.

> *Your answer here.*

### 4b. Why drain the buffer before releasing the semaphore?
Even with the wide lock, you still have to flush leftover bytes before the final `semop` (the +1 release). Explain — again at the byte level — what goes wrong if you don't.

> *Your answer here.*

---

## 5. Performance Comparison: Threads+Mutex vs Processes+Semaphore

Run each configuration **three times** and report the median of `elapsed_ms`:

| config              | part2 (median ms) | part3 (median ms) |
|---|---|---|
| `"test" 1 1000`     |  |  |
| `"test" 50 100`     |  |  |
| `"test" 50 1000`    |  |  |

Address:
- Which is faster, and why?
- How does spawn cost (`fork` vs `pthread_create`) factor in?
- How does sync-primitive cost (in-process mutex vs kernel semaphore syscall) factor in?
- At which configuration is each cost most visible?

> *Your answer here.*