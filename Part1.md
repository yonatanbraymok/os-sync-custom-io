# Part 1 — The Baseline (Naive Processes)

## Objective
Demonstrate, in your own working code, the chaos that arises when concurrent processes write to the same file descriptor without any synchronization.

## Architecture
1. The **parent** sets up the redirection (open + dup2) and parses argv.
2. The parent records the start timestamp.
3. The parent loops `num_workers` times and calls `fork()` each iteration.
4. Each child runs the worker routine: a loop of `writes_per_worker` iterations, each iteration formats a message via `snprintf` and calls `my_write`. After the loop, the child calls `flush_buffer` and `_exit(0)`.
5. The parent waits for every child via `wait()` / `waitpid()`.
6. The parent records the end timestamp and reports it to **stderr**.

## Required: Fork All Before Wait
You **must** fork all `num_workers` children before calling any `wait()`. The following pattern is forbidden because it serializes execution and hides the chaos:

```c
// FORBIDDEN
for (int i = 0; i < num_workers; i++) {
    pid = fork();
    if (pid == 0) { worker(...); _exit(0); }
    waitpid(pid, NULL, 0);   // <— this waits BEFORE forking the next child
}
```

The correct pattern is:

```c
pid_t pids[num_workers];
for (int i = 0; i < num_workers; i++) {
    pids[i] = fork();
    if (pids[i] == -1) { perror("fork"); /* ... */ }
    if (pids[i] == 0) { worker(i, message, writes_per_worker); _exit(0); }
}
for (int i = 0; i < num_workers; i++) {
    if (waitpid(pids[i], NULL, 0) == -1) { perror("waitpid"); /* ... */ }
}
```

## Synchronization
**None.** No mutexes, no semaphores, no lock files, no `sleep` to space out writes. The whole point of this part is to observe what happens *without* synchronization.

You **may** use `wait()` at the end, of course — that is reaping, not synchronization.

## Workers Use `_exit`, Not `exit`
A child should call `_exit(0)` after `flush_buffer()`, **not** `exit(0)`. `exit()` runs `atexit` handlers and stdio cleanup, which can flush stdio buffers in a way that interacts badly with our manual buffering. (Try replacing `_exit` with `exit` later and see what changes — it's an interesting experiment.)

## Expected Output
With `CHUNK_SIZE=64` and a typical message of 10–20 chars, you should observe in `output.txt`:

- Some lines that are perfectly well-formed.
- Some lines that contain the *tail* of one worker's message immediately followed by the *head* of another's, with no `\n` separator.
- Some lines whose `[Type: …] [ID: …] [Seq: …] …` header is itself sliced across two workers' interleaved chunks.
- The total byte count will be exactly the same as a fully synchronized run (no bytes are lost — they're just interleaved).

This is **not** a bug in your code. It is the system telling you the truth about what happens when multiple processes share a file offset and nobody is mediating.

## Optional: What to Put in REPORT.md for Part 1
*(REPORT.md is optional; filling it in earns up to +5 bonus points capped at 100. Skip this section if you don't plan to fill in the report.)*
Run:
```bash
./part1 "Hello concurrent world" 4 50
```
1. Paste a 5–10-line excerpt from `output.txt` that clearly shows interleaving.
2. In one paragraph, explain *why* the interleaving happens. Specifically address: where does the buffer live in memory after `fork()`? Where does the file offset live? Which one is shared?

## Acceptance Criteria (Autograder)
- Compiles cleanly with `-Wall -Wextra` (no warnings).
- Exits with status 0 on valid input.
- Total output bytes equal `num_workers × writes_per_worker × len(formatted_line)`.
- Every fork/waitpid/dup2/open/write call is `perror`-checked.W

## Forbidden in Part 1
- `pthread_*`
- `sem_*`
- File-locking (`flock`, `fcntl(F_SETLK)`, lockfiles, …)
- `sleep`/`usleep` used as a synchronization device. (Sleeping for debugging output is fine but pointless here.)
- Any form of `fopen`/`fprintf`/`puts`/etc. for `output.txt`.
