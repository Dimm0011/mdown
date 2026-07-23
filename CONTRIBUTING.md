# Contributing to mdown

Thanks for your interest in contributing!

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
make -j$(nproc)
```

## Running Tests

```bash
cd build
ctest --output-on-failure
```

Or individually:
```bash
./test_all      # unit tests
./gtest_all     # Google Test
./catch2_all    # Catch2
```

## Code Style

- C++17
- 4-space indentation
- No comments unless non-obvious logic
- Follow existing naming conventions

## Submitting Changes

1. Fork the repository
2. Create a branch (`git checkout -b feature/my-feature`)
3. Make your changes
4. Run tests — all must pass
5. Open a Pull Request

## Reporting Issues

Use GitHub Issues. Include:
- OS and compiler version
- Steps to reproduce
- Expected vs actual behavior
