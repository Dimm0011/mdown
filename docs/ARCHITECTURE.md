# Architecture

## Overview

MultiDow splits files into byte-range chunks and downloads them in parallel using multiple threads (one CURL handle per thread). Each thread writes to a shared file at a fixed offset, protected by a mutex.

```
URL ──► probe_server() ──► file_size, range_supported
       │
       ├── num_threads=1 or no Range ──► download_single()
       │
       └── num_threads=N ──► start_download()
                               │
                               ├── thread 0: Range: 0-249999
                               ├── thread 1: Range: 250000-499999
                               └── thread 2: Range: 500000-749999
```

## Chunk Splitting

File of size `S` with `N` threads:

- `chunk_size = S / N`
- Thread `i` downloads bytes `[i * chunk_size, (i+1) * chunk_size - 1]`
- Last thread gets the remainder: `[i * chunk_size, S - 1]`

Each thread sends an HTTP `Range` header (e.g. `Range: 250000-499999`). The server responds with `206 Partial Content`.

## Write Model

Each thread writes directly to the output file using `fseek` to its assigned offset. A `std::mutex` serializes file access:

```
ChunkCtx {
    write_offset   — current byte position in the file
    bytes_written  — bytes downloaded in this session (for progress)
    fp_mtx         — mutex protecting the shared FILE*
}
```

The write callback: lock → fseek → fwrite → update offset → unlock.

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

Each thread independently checks its position in the file via `ftell` and resumes from there:

```
Thread 0: Range: 2500000-24999999  (was interrupted at 10MB)
Thread 1: Range: 25000000-49999999 (was interrupted at 30MB)
```

## Network Recovery

On connection loss, each thread retries with exponential backoff:

```
retry 0 → wait 1s → retry 1 → wait 2s → retry 2 → wait 4s → ... (max 16s)
```

On each retry, the thread re-reads its file position and sends a new `Range` header. This means recovery works transparently across network outages of any duration.

## Metadata Lifecycle

```
start_download()
  ├── file already complete? → "Already downloaded", return
  ├── no .mdow file? → create fresh
  └── .mdow exists? → resume from partial data
       │
       ├── all threads succeed → delete .mdow → return true
       ├── any thread fails → save .mdow → return false
       └── Ctrl+C → save .mdow → return false
```

## Progress Display

One line per file, updated via ANSI escape codes:

```
\033[<N>A   — move cursor up N lines (erase previous state)
\033[2K     — clear entire line
```

Output is buffered into a single `std::ostringstream` and written in one `std::cerr` call to prevent interleaving. Throttled to max 1 redraw per 150ms.
