# How to Contribute

## CI/CD

You must conform to CI/CD requirements. See [CI.md](CI.md) for details.

A code change is not ready until the local validation gates pass. Run them in the fail-fast order documented under [Local Validation Gates](CI.md#local-validation-gates): format, clang-tidy, then the complete `run_tests.py` suite. Do not substitute a focused test for the complete suite when declaring work finished.

## Writing Tests

Use the TestCase system for in-process functional verification. See [Test.md](Test.md) for how to write test cases and run them locally before pushing.

* Create new test cases that exactly test a new feature or reproduce a bug. Improve existing test cases when necessary.
* Do not ignore any crashes or errors. Fix them before continuing; if one is unrelated to the current task, report it and record it in [TODO.md](TODO.md).

## Coding Style

Follow the coding style already used in the codebase.

* **Modern C++20**: Use newest features (concepts, ranges, std::format, etc.)
* **Self-documenting code**: Avoid comments for obvious code; comment only structural design, algorithms, and non-obvious caveats. Comments state present facts, never narrate changes or history. If code needs a comment to be understood, reconsider the implementation first.
* **No dead code**: Remove unused code, arguments, variables, includes immediately
* **Clean design**: Prefer simplicity and readability over cleverness
* **No over-engineering**: Only implement what's requested; avoid speculative abstractions
* **Don't repeat yourself**: Reuse existing code where possible; when a snippet appears twice, extract a shared function. Prefer calling a high-level wrapper over its low-level internals.
* **Surgical changes**: Every changed line should trace to the task at hand. Do not reformat or restyle code beyond your change.
* **Cross-platform paths**: Use the `FileManager` abstraction, never raw path separators
* **RHI resources**: Use the deferred deletion pattern for GPU resource cleanup

### Formatters

Always use formatters in the environment or IDE to format code and documents after edit; do not format manually. Read diagnostic errors/warnings from the IDE.

* c++/objc/slang: `.clang-format` (clang-format)
* markdown: `.markdownlint.json` (markdownlint)
* python: PEP8 (autopep8, configured in `pyproject.toml`)
* All code must pass clang-tidy with the project's [.clang-tidy](../.clang-tidy) configuration. All warnings are treated as errors (`WarningsAsErrors: "*"`).

CI enforces formatting on every push and PR, and clang-tidy on every push and PR that touches code. See [CI.md](CI.md) for the matching local commands.

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

Avoid nested ternary operators (`readability-avoid-nested-conditional-operator`).

## Documentation

Committed docs are a user manual and a design doc, not a work log. Assume the reader is a user or developer seeing the project for the first time; write only what they would want to know.

* State present-tense facts: available knobs with defaults and tradeoffs, how to measure and debug, and settled design constraints.
* Never narrate history: no change records, experiment stories, or dates. Write negative results as present constraints ("X cannot help because Y"), not as findings.
* Never reference paths readers cannot see (gitignored or temporary files).
* Update docs in the same change that alters the behavior they describe.
* Do not repeat content across docs; keep each topic in a dedicated file and link to it.
* Do not insert new-lines inside a paragraph.
