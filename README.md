# MultiDow

Multi-threaded file downloader with resume support for Linux.

## Features

- Multi-threaded download — splits file into chunks, downloads in parallel
- Multi-file — download multiple files simultaneously
- Resume — interrupted downloads continue from where they left off
- Auto-retry — stalled transfers detected and retried automatically
- Progress bar — real-time progress for each file (apt-style)
- SHA-256 checksum verification
- Full HTTP support — redirects, cookies, User-Agent

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
- C++17 compiler

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
