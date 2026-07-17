# Changelog

## [1.0.0] - 2025-07-17

### Added
- Multi-threaded file downloading with configurable thread count
- Resume interrupted downloads via `.mdow` metadata files
- Automatic resume on network failure with exponential backoff
- HTTP Range request support for parallel chunk downloads
- SHA-256 checksum verification (`-c` flag)
- Multiple URL support via `-f urls.txt` or positional arguments
- apt/dnf-style progress bars with ETA and speed
- HTTP redirects and cookies support
- Low-speed detection (auto-abort stalled transfers)
- Ctrl+C graceful shutdown with progress save
- 3 test suites: unit (20), Google Test (39), Catch2 (33)
- CI/CD via GitHub Actions (Ubuntu, macOS) with clang-format/clang-tidy
- Configurable timeout, retries, thread count
- Architecture documentation (`docs/ARCHITECTURE.md`)
- CONTRIBUTING.md, issue/PR templates
- Semver tagging (v1.0.0)
