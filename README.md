# Synchronization & Custom I/O (OS Assignment 2)

The same task solved three times with different OS concurrency paradigms: N workers each write a message M times to `output.txt` through a hand-rolled 64-byte stdio-style buffer (`my_write`), using `open` + `dup2` + raw `write()`.

| Part | Paradigm | What it shows |
|------|----------|---------------|
| `part1.c` | `fork()`, no sync | Interleaved/corrupted output from unsynchronized writers |
| `part2.c` | `pthread` + mutex | Protecting shared **memory** (the buffer) |
| `part3.c` | `fork()` + System V semaphore | Protecting a shared **OS resource** (the file offset) |

## Build & run
```bash
cd src && make
./part2 "Hello" 4 100    # <message> <num_workers> <writes_per_worker>
cat output.txt
```
Sanity check: `python3 tests/sanity.py`

See [ASSIGNMENT.md](ASSIGNMENT.md) and `Part1-3.md` for the full spec, and [LECTURE.md](LECTURE.md) for study notes.
