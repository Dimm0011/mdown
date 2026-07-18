# MultiDow

Multi-threaded file downloader with resume support for Linux.

## Features

- Multi-threaded download — splits file into chunks, downloads via shared thread pool
- Multi-file — download multiple files simultaneously
- Resume — interrupted downloads continue from where they left off
- Auto-retry — stalled transfers detected and retried automatically
- Progress bar — real-time progress for each file (apt-style)
- SHA-256 checksum verification
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
5. `Resume manager` saves/loads `.mdow` metadata for interrupted downloads
6. `Progress manager` aggregates per-chunk progress into a single line per file, timer-driven redraw (100ms), sliding window speed
7. `Checksum verifier` validates SHA-256 after download completes

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
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
# Single file
./multidow "https://example.com/file.zip"

# Multiple files
./multidow "https://url1.zip" "https://url2.zip" "https://url3.zip"

# From URL list file
./multidow -f urls.txt

# With options
./multidow -f urls.txt -t 8 -r 5
```

## Options

| Option | Description | Default |
|--------|-------------|---------|
| `-f, --file <path>` | File with URLs (one per line, `#` for comments) | — |
| `-o, --output <path>` | Output filename (single URL only) | from URL |
| `-t, --threads <N>` | Download threads per file | 4 |
| `-c, --checksum <SHA256>` | Verify SHA-256 after download | — |
| `-r, --retries <N>` | Max retries per chunk | 3 |
| `-T, --timeout <sec>` | Transfer timeout | 300 |
| `-h, --help` | Show help | — |

## URL list file format

```text
# Comments start with #
https://example.com/file1.zip
https://example.com/file2.iso
https://example.com/file3.tar.gz
```

## Resume

If download is interrupted (Ctrl+C, network failure, etc.), just run the same command again. MultiDow saves resume metadata (`.mdow` files) and continues from where it stopped.

```bash
# First attempt — interrupted at 60%
./multidow "https://example.com/large-file.zip"
# ^C

# Second attempt — resumes from 60%
./multidow "https://example.com/large-file.zip"
```

## Tests

### Simple tests (no dependencies)

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
ctest --output-on-failure
```

### Google Tests

Google Test downloads automatically via CMake FetchContent on first build:

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
ctest --output-on-failure
# or run directly:
./gtest_all
```

| Suite | Tests | Covers |
|-------|-------|--------|
| FormatBytes | 8 | byte formatting (B/KB/MB/GB) |
| FormatSpeed | 3 | speed formatting |
| MakeBar | 8 | progress bar rendering |
| ChecksumTest | 7 | SHA-256, verify checksum |
| ProgressTest | 13 | ProgressManager lifecycle, multi-file, errors |

## License

MIT
