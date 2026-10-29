[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/-W7xRGmh)
# Exercise 2 — Synchronization & Custom I/O

## Due Date
26.05.2026

## Overview
You will solve the same problem **three times**, each time with a different operating-system paradigm:

1. **Part 1 — Naive Processes:** observe the chaos of unsynchronized concurrent file writes.
2. **Part 2 — Threads + Mutex:** synchronize a shared *memory* space.
3. **Part 3 — Processes + System V Semaphore:** synchronize a shared *OS resource* (the file offset).

By the end of the assignment you will understand:
- How `dup2()` redirection works (`open` + `dup2` + write to `stdout`).
- How a stdio-style buffer works under the hood (and why one is needed).
- The crucial difference between protecting **memory** (mutex around buffer state) and protecting an **OS resource** (semaphore around an inter-process write path).

> **Schedule note.** Parts 1 and 2 use only material covered up to the **fork/exec/wait** and **threads/POSIX/mutex** sessions. Part 3 requires **semaphores**, which is the final TA session before the assignment is due. You are strongly encouraged to finish Parts 1 and 2 *before* the semaphores lecture.

## Repository Layout
```
.
├── README.md            — this file (read FIRST — contains the shared requirements)
├── Part1.md             — naive processes
├── Part2.md             — threads + mutex
├── Part3.md             — processes + named semaphore
├── REPORT.md            — optional report (small bonus available)
├── src/
│   ├── part1.c          — your code (stub provided)
│   ├── part2.c          — your code (stub provided)
│   ├── part3.c          — your code (stub provided)
│   └── Makefile
└── tests/
    └── sanity.py        — sanity check; run before submitting
```

## How to Read This Assignment
1. Read this README **end to end** — sections "Shared Requirements" through "Forbidden / Allowed" apply to all parts.
2. Then read `Part1.md`, `Part2.md`, `Part3.md` in order.
3. Implement the parts in order. The three `partN.c` files are independent (each has its own `main`), but they share the same buffer logic, output format, command-line interface, and error-handling rules. **Reuse your own code** between parts — only the synchronization layer should change.

## Build & Run
```bash
cd src
make                    # builds part1, part2, part3
./part1 "Hello" 4 100   # runs part1 with 4 workers writing 100 messages each
cat output.txt          # see what happened
```

## Sanity Check
Before submitting, run:
```bash
python3 tests/sanity.py
```
This compiles your code and runs each part with small inputs, confirming the basics: it builds without warnings, the binaries run, the output has the right shape, and the timing line is on stderr.

**This is not a grader.** Passing it just means your code is not obviously broken — it doesn't guarantee a passing grade. Edge cases, stress loads, and forbidden-API checks happen separately during grading.

## Deliverables
Push to your GitHub Classroom repository before the deadline:
1. id.txt - a .txt file containing the last 5 digits of your ID
2. `src/part1.c`, `src/part2.c`, `src/part3.c` — your three implementations.
3. `src/Makefile` — must build all three with `make`.
4. *(Optional)* `REPORT.md` — fill in for up to +5 bonus points (capped at 100 total).

Do **not** commit binaries (`part1`, `part2`, `part3`) or `output.txt`. A `.gitignore` is provided.

---

# Shared Requirements

These apply to **all three parts**. Each `partN.c` is an independent file with its own `main()`, but the buffer logic, output format, command-line interface, timing, and error-handling rules below are identical across all three. The only thing that differs between parts is the synchronization layer (and processes vs threads).

## 1. System-Level Output Redirection

You may **not** use `fopen`, `fprintf` (to anything other than `stderr`), `fwrite`, or any other stdio function for writing to `output.txt`.

The parent process must:
1. Open `output.txt` with the raw `open()` system call:
   ```c
   int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
   ```
2. Redirect `STDOUT_FILENO` (file descriptor `1`) to that file:
   ```c
   if (dup2(fd, STDOUT_FILENO) == -1) { perror("dup2"); /* ... */ }
   close(fd);
   ```
3. From that point on, **every byte written via `write(STDOUT_FILENO, …)` ends up in `output.txt`.**

This redirection happens in the parent **before** any child process is forked or any thread is created, so all workers inherit the redirected stdout.

> Why this matters: when you `fork()`, the child inherits a *copy* of the file-descriptor table that points to the **same entry in the open file table** (and therefore the same offset). Multiple processes writing to fd 1 are all racing on the same offset. This is the exact phenomenon Part 1 demonstrates and Part 3 fixes.

## 2. The Custom Buffer

You are forbidden from calling `write()` once per character or once per message. You must implement a fixed-size buffering layer.

### Required globals (do not rename or resize)
```c
#define CHUNK_SIZE 64
char buffer[CHUNK_SIZE];
int  buffer_pos = 0;
```

### `my_write(const char *str)`
Copies the bytes of `str` (excluding the terminating `\0`) into `buffer`. Behaviour:

- Copy as many bytes as fit into the remaining space in `buffer`.
- **Flush rule:** call the actual `write(STDOUT_FILENO, buffer, CHUNK_SIZE)` system call **only when the buffer becomes completely full** (i.e. `buffer_pos == CHUNK_SIZE`). After flushing, reset `buffer_pos = 0`.
- **Partial writes:** if the input string is larger than the remaining space in the buffer, fill the buffer to the brim, flush it, and then continue copying the remaining bytes (recursing if necessary — the rest of the string itself may be larger than `CHUNK_SIZE`).

### `flush_buffer(void)`
Writes the *first `buffer_pos` bytes* of `buffer` to `STDOUT_FILENO` (this is the "non-full flush" used at end of execution). Resets `buffer_pos` to `0`. Must handle short writes (i.e. `write()` returning fewer bytes than requested) by looping.

### Why `CHUNK_SIZE = 64`?
A formatted line (see §3) is roughly 50 bytes long. With a 64-byte chunk, **most flushes will split a message**: the boundary between two flushes will fall in the middle of a line. This is intentional. Without it, Part 1's chaos would be invisible (each message would happen to fit cleanly in one `write()` call).

You will revisit `CHUNK_SIZE` in your performance analysis (see `REPORT.md`, optional).

## 3. The Output Format (STRICT)

Every line written by a worker must be produced by `snprintf` into a local buffer and then passed to `my_write`. The format is:

```
[Type: <PROCESS|THREAD>] [ID: <worker_id>] [Seq: <seq_number>] <message>\n
```

Field rules:
| Field | Format specifier | Notes |
|---|---|---|
| Type | `PROCESS` or `THREAD` literal | `PROCESS` for parts 1 & 3; `THREAD` for part 2. Uppercase. |
| ID   | `%d` | 0-indexed: `0 .. num_workers-1`. |
| Seq  | `%03d` | 0-indexed, zero-padded to 3 digits: `000 .. writes_per_worker-1`. |
| message | `%s` | The user-supplied message string, no trailing newline. |

Example:
```
[Type: THREAD] [ID: 2] [Seq: 045] Hello, world!
```

Each line is validated against this exact regex:
```
^\[Type: (PROCESS|THREAD)\] \[ID: (\d+)\] \[Seq: (\d{3})\] (.+)\n$
```

> Note the exact spacing: one space inside each `[ … ]` separator, single spaces between fields, single `\n` at end. `\t` will fail. Trailing spaces will fail.

## 4. Command-Line Arguments

Every part's `main()` must accept exactly:

```
./partN <message> <num_workers> <writes_per_worker>
```

- `<message>` — an arbitrary string, *guaranteed not to contain* `\n` or NUL.
- `<num_workers>` — a positive integer.
- `<writes_per_worker>` — a positive integer.

If `argc != 4`, print
```
Usage: <argv[0]> <message> <num_workers> <writes_per_worker>
```
to **stderr** and exit with status `1`.

Reject non-positive integers with a clear error message to stderr and exit status `1`.

## 5. Timing & Performance Reporting

The parent process must:
1. Record a start timestamp **just before** spawning workers, using `gettimeofday()` or `clock_gettime(CLOCK_MONOTONIC, …)`.
2. Record an end timestamp **just after** all workers have finished and all buffers are flushed.
3. Print the elapsed wall-clock time in **milliseconds** to **`stderr`** (so it doesn't pollute `output.txt`):
   ```
   elapsed_ms=12.345
   ```
   The exact format is `elapsed_ms=<float>` with 3 decimal places.

## 6. Error Handling

Every system call (`open`, `dup2`, `fork`, `pthread_create`, `pthread_join`, `pthread_mutex_init`, `semget`, `semop`, `semctl`, `ftok`, `wait`, `waitpid`, `write`, `read`, `close`, …) must be checked. On failure, call `perror("<syscall-name>")` and exit with a non-zero status.

A "checked" call looks like this:
```c
pid_t pid = fork();
if (pid == -1) { perror("fork"); exit(1); }
```

## 7. Cleanup

The program must leave the system in a clean state:
- All file descriptors opened by the parent are closed before exit.
- All threads are joined (Part 2).
- All children are reaped via `wait()` / `waitpid()` (Parts 1 & 3) — **no zombies**.
- All mutexes are destroyed (Part 2).
- The System V semaphore is removed via `semctl(semid, 0, IPC_RMID)` by the parent (Part 3) so it does not linger in the kernel IPC table. After exit, `ipcs -s` should show no leftover semaphore.

---

## Forbidden / Allowed (Summary)

- ❌ `fopen`, `fprintf` (to anything other than `stderr`), `fwrite`, `fputs`, `puts`, `printf`. You must use `open` + `dup2` + raw `write()` (wrapped by `my_write`).
- ❌ Any third-party libraries.
- ❌ Locks of any kind in Part 1.
- ❌ `pthread_*` in Parts 1 and 3.
- ❌ POSIX `sem_*` (`sem_open`, `sem_wait`, `sem_post`, etc.) anywhere — Part 3 uses **System V** semaphores (`semget`, `semop`, `semctl`).
- ❌ System V semaphore calls (`semget`, `semop`, `semctl`) in Parts 1 and 2.
- ❌ File-locking (`flock`, `fcntl(F_SETLK)`) anywhere.
- ✅ `fprintf(stderr, …)` is fine (e.g., for the timing message and error reporting).
- ✅ `snprintf` for formatting strings before passing them to `my_write`.
- ✅ Reusing your own `my_write` / `flush_buffer` code across the three parts.

## Submission Workflow (GitHub Classroom)
1. Accept the assignment via the Classroom link.
2. Clone your repository.
3. Implement, test locally with `python3 tests/sanity.py`, commit, and push.

Good luck — and remember: **the chaos in Part 1 is a feature, not a bug**. Stare at it until you understand exactly *why* it happens. That understanding is what Parts 2 and 3 are there to fix.
