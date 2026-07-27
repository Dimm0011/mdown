# mdown

Multi-threaded file downloader with resume support for Linux.

## Features

- Multi-threaded download — splits file into chunks, downloads via shared thread pool
- Multi-file — download multiple files simultaneously
- Resume — interrupted downloads continue from where they left off (per-chunk tracking)
- Auto-retry — stalled transfers detected and retried automatically
- Progress bar — real-time progress for each file (apt-style)
- SHA-256 checksum verification (case-insensitive)
- Full HTTP support — redirects, cookies, User-Agent
- Cooperative cancellation — Ctrl+C saves progress cleanly via `std::stop_token`

## Architecture

```
┌─────────────────────────────────────────────────┐
│                CLI (main.cpp)                   │
│  arg parsing, signal handler (SIGINT/SIGTERM)   │
│  spawns std::jthread per URL                    │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│             Downloader (downloader.cpp)          │
│  orchestrates probe, chunk split, retry, resume  │
└──┬──────────┬───────────┬───────────┬───────────┘
   │          │           │           │
┌──▼───┐ ┌───▼────┐ ┌────▼────┐ ┌───▼───────────┐
│ CURL │ │ Chunk  │ │ Resume  │ │  Progress     │
│ wrap │ │ pool   │ │ manager │ │  manager      │
│ per  │ │ submit │ │ .mdow   │ │  per-file bar │
│ req  │ │ to     │ │ files   │ │  ETA / speed  │
│      │ │ shared │ │         │ │  std::format  │
└──────┘ │ pool   │ └─────────┘ └───────────────┘
         └────────┘
                 ┌───────────────────────┐
                 │ ThreadPool            │
                 │ std::jthread workers  │
                 │ shared across files   │
                 └───────────┬───────────┘
                             │
                 ┌───────────▼───────────┐
                 │ Checksum              │
                 │ verifier (SHA-256)    │
                 │ OpenSSL EVP           │
                 └───────────────────────┘
```

**Data flow:**
1. `CLI` parses args, installs signal handler, spawns one `std::jthread` per URL
2. Each `Downloader` probes the server (HEAD request) for file size & Range support
3. `start_download()` splits file into N chunk tasks, submits each to the shared `ThreadPool`
4. Each chunk uses its own `CURL` handle with a `Range` header; returns `std::future<bool>`
5. `Resume manager` saves/loads `.mdow` metadata with per-chunk progress for interrupted downloads
6. `Progress manager` aggregates per-chunk progress into a single line per file, timer-driven redraw (100ms), sliding window speed
7. `Checksum verifier` validates SHA-256 after download completes

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Dependencies

- libcurl (with SSL)
- OpenSSL
- CMake 3.14+
- C++20 compiler (GCC 13+ or Clang 16+)

```bash
# Ubuntu/Debian
sudo apt install libcurl4-openssl-dev libssl-dev cmake g++
```

## Usage

```bash
./mdown "https://example.com/file.zip"
./mdown "https://url1.zip" "https://url2.zip" "https://url3.zip"
./mdown -f urls.txt
./mdown -f urls.txt -t 8 -r 5
```

## Options

| Option | Description | Default |
|--------|-------------|---------|
| `-f, --file <path>` | File with URLs (one per line, `#` for comments) | — |
| `-o, --output <path>` | Output filename (single URL only) | from URL |
| `-t, --threads <N>` | Download threads per file (capped at hardware_concurrency, max 16) | 4 |
| `-c, --checksum <SHA256>` | Verify SHA-256 after download (case-insensitive) | — |
| `-r, --retries <N>` | Max retries per chunk | 3 |
| `-T, --timeout <sec>` | Transfer timeout | 300 |
| `-h, --help` | Show help | — |

## Resume

If download is interrupted (Ctrl+C, network failure, etc.), run the same command again. mdown saves per-chunk progress in `.mdow` metadata and resumes exactly where it stopped — including multi-threaded mode.

```bash
./mdown "https://example.com/large-file.zip"
# ^C
./mdown "https://example.com/large-file.zip"
# Resumes from saved chunk offsets
```

## Tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
./build/gtest_all
```

63 tests covering: format, progress bar, SHA-256, ProgressManager lifecycle, downloader (mock transport).

## License

MIT
