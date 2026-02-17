# How to Contribute

## CI/CD

You must conform to CI/CD requirements. See [CI.md](CI.md) for details.

## Coding Style

Please follow the coding style already used in the codebase.

### Formatters

* c++/objc/slang: `.clang-format` (clang-format)
* markdown: `.markdownlint.json` (markdownlint)
* python: PEP8 (autopep8)

### Static Analysis (clang-tidy)

All C++ code must pass clang-tidy with the project's [.clang-tidy](../.clang-tidy) configuration. All warnings are treated as errors (`WarningsAsErrors: "*"`).
