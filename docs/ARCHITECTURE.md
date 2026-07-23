# Architecture

## Overview

mdown splits files into byte-range chunks and downloads them in parallel via a shared `ThreadPool`. Each file runs on its own `std::jthread` (cooperative cancellation via `std::stop_token`). Individual chunk tasks are submitted to the pool and return `std::future<bool>`. Each chunk uses its own CURL handle and writes to the output file at a fixed offset, protected by a per-file mutex.

```
main()
  │
  ├── spawns one std::jthread per URL
  │     └── Downloader::run()
  │           ├── probe_server()         — HEAD request, file size & Range support
  │           ├── start_download()       — multi-thread path (N chunks via ThreadPool)
  │           │     └── pool_.submit()   — N tasks, each returns std::future<bool>
  │           │           └── download_chunk() — own CURL handle, Range header, writes to shared FILE*
  │           ├── download_single()      — single-thread fallback (no Range support)
  │           └── checksum verification  — SHA-256 via OpenSSL EVP
  │
  ├── ProgressManager                    — one line per file, ANSI cursor, std::format
  └── ThreadPool                         — shared across all files, std::jthread workers
```

### Concurrency model

- **File-level**: each file runs on a `std::jthread` in `main()`. This isolates files from each other — a slow file never blocks a fast one.
- **Chunk-level**: within each file, N chunk tasks are submitted to the shared `ThreadPool`. Chunks from different files share the same pool, keeping total thread count bounded.
- **Why not pool-within-pool?**: submitting chunk tasks from a pool task would risk thread starvation (all pool threads blocked waiting for sub-tasks that can never run). File-level threads are separate `std::jthread`s to prevent deadlock.

```
ThreadPool (4 workers)          std::jthread (1 per file)
┌──────────────────────┐        ┌─────────────────────────┐
│ worker 0: chunk A-0  │◄──────│ file 1 thread: submit()  │
│ worker 1: chunk B-0  │◄──────│ file 2 thread: submit()  │
│ worker 2: chunk A-1  │◄──────│ file 1 thread: submit()  │
│ worker 3: chunk B-1  │◄──────│ file 2 thread: submit()  │
└──────────────────────┘        └─────────────────────────┘
```

## Chunk Splitting

File of size `S` with `N` chunk tasks:

- `chunk_size = S / N`
- Chunk `i` downloads bytes `[i * chunk_size, (i+1) * chunk_size - 1]`
- Last chunk gets the remainder: `[i * chunk_size, S - 1]`

Each chunk sends an HTTP `Range` header (e.g. `Range: 250000-499999`). The server responds with `206 Partial Content`.

## Write Model

Each chunk writes directly to the output file using `fseek` to its assigned offset. A `std::mutex` (per file, not global) serializes file access:

```
ChunkCtx {
    write_offset   — current byte position in the file
    bytes_written  — bytes downloaded in this session (for progress)
    fp_mtx         — mutex protecting the shared FILE*
}
```

The write callback: lock → fseek → fwrite → update offset → unlock → update progress (outside lock).

Progress updates are called outside the file-write mutex to prevent terminal I/O from blocking other chunks' disk writes.

## Cancellation (C++20)

The signal handler is async-signal-safe — it only sets `volatile sig_atomic_t g_signal_received = 1`. A dedicated timer thread polls this flag and calls `g_stop.request_stop()` on the global `std::stop_source`. All download loops check `g_stop.stop_requested()` and break early. The CURL xfer callbacks also check it and return 1 to abort the transfer.

```
signal(SIGINT) → g_signal_received = 1       (async-signal-safe)
                       │
                       ▼
timer thread: g_stop.request_stop()
                       │
                       ├── download_chunk(): checks before each retry
                       ├── download_single(): checks before each retry
                       ├── chunk_xfer_cb(): returns 1 to abort CURL
                       └── single_xfer_cb(): returns 1 to abort CURL
```

`std::jthread` auto-joins on destruction — no manual `join()` calls needed.

## Resume (.mdow format)

When a download is interrupted (Ctrl+C, network failure, crash), a `.mdow` metadata file is created:

```
<url>
<file_size>
<output_path>
```

On next run, if `<output_path>.mdow` exists alongside the partial file:

1. Open file in `r+b` mode (preserve existing data)
2. Read current file size to determine how much was already downloaded
3. Send adjusted `Range` headers starting from the last written byte
4. On completion, delete the `.mdow` file

### Single-thread resume

```
File: 100MB partially downloaded (45MB)
→ open "r+b", seek to 45MB
→ Range: 45000000-99999999
```

### Multi-thread resume

Each chunk independently checks its file position via `ftell` and resumes from there:

```
Chunk 0: Range: 2500000-24999999  (was interrupted at 10MB)
Chunk 1: Range: 25000000-49999999 (was interrupted at 30MB)
```

## Network Recovery

On connection loss, each chunk retries with exponential backoff:

```
retry 0 → wait 1s → retry 1 → wait 2s → retry 2 → wait 4s → ... (max 16s)
```

On each retry, the chunk re-reads its file position and sends a new `Range` header. This means recovery works transparently across network outages of any duration.

## Metadata Lifecycle

```
start_download()
  ├── file already complete? → "Already downloaded", return
  ├── no .mdow file? → create fresh
  └── .mdow exists? → resume from partial data
       │
       ├── all chunks succeed → delete .mdow → return true
       ├── any chunk fails → save .mdow → return false
       └── Ctrl+C → save .mdow → return false
```

## Progress Display

One line per file, updated via ANSI escape codes:

```
\033[<N>A   — move cursor up N lines (erase previous state)
\033[2K     — clear entire line
\r          — return cursor to start of line
```

A dedicated `std::jthread` timer polls `ProgressManager::poll()` every 100ms. Individual `update_thread()` calls only update data and set a `dirty_` flag — they do not trigger redraws. State transitions (`mark_thread_active`, `mark_thread_finished`, `mark_thread_error`) call `do_redraw()` immediately.

Speed is computed from a sliding window of the last 2 seconds of byte samples, not the cumulative average from download start.

Output is buffered into a single `std::string` via `std::format` and written in one `std::cerr` call to prevent interleaving. When no Content-Length is available, the display shows the amount downloaded so far (e.g. `downloading... 45.2 MB`).
