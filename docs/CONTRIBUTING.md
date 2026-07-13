# How to Contribute

## CI/CD

You must conform to CI/CD requirements. See [CI.md](CI.md) for details.

A code change is not ready until the local validation gates pass. Run them in the
fail-fast order documented under [Local Validation Gates](CI.md#local-validation-gates):
format, clang-tidy, then the complete `run_tests.py` suite. Do not substitute a
focused test for the complete suite when declaring work finished.

## Writing Tests

Use the TestCase system for in-process functional verification. See [Test.md](Test.md)
for how to write test cases and run them locally before pushing.

## Coding Style

Follow the coding style already used in the codebase.

* **Modern C++20**: Use newest features (concepts, ranges, std::format, etc.)
* **Self-documenting code**: Avoid comments for obvious code; comment only structural design, algorithms, and non-obvious caveats
* **No dead code**: Remove unused code, arguments, variables, includes immediately
* **Clean design**: Prefer simplicity and readability over cleverness
* **No over-engineering**: Only implement what's requested; avoid speculative abstractions

### Formatters

Always use formatters in the environment or IDE to format code and documents after edit. Read diagnostic errors/warnings from the IDE.

* c++/objc/slang: `.clang-format` (clang-format)
* markdown: `.markdownlint.json` (markdownlint)
* python: PEP8 (autopep8, configured in `pyproject.toml`)
* All code must pass clang-tidy with the project's [.clang-tidy](../.clang-tidy) configuration. All warnings are treated as errors (`WarningsAsErrors: "*"`).

CI enforces formatting on every push and PR, and clang-tidy on every push and PR
that touches code. See [CI.md](CI.md) for the matching local commands.

### Naming Conventions

All C++ code must follow the `.clang-tidy` naming rules. Key conventions:

| Construct                              | Style                        | Example                 |
| -------------------------------------- | ---------------------------- | ----------------------- |
| Namespace                              | `lower_case`                 | `sparkle`               |
| Class / Struct / Enum                  | `CamelCase`                  | `RenderFramework`       |
| Function                               | `CamelCase`                  | `BeginFrame()`          |
| Local variable                         | `lower_case`                 | `frame_count`           |
| Public member                          | `lower_case`                 | `width`                 |
| Private / protected member             | `lower_case_` (trailing `_`) | `render_thread_`        |
| `constexpr` / static / global constant | `CamelCase`                  | `MaxBufferedTaskFrames` |
| Enum constant                          | `CamelCase`                  | `PrimaryLeft`           |

All warnings are treated as errors (`WarningsAsErrors: "*"` in `.clang-tidy`).
Avoid nested ternary operators (`readability-avoid-nested-conditional-operator`).
