# Contributing

## Workflow

* A change is ready when the [validation gates](#validation-gates) pass locally; [CI](CI.md) then repeats them across every platform before merge.
* Add a test case that exercises the new feature or reproduces the bug, or improve an existing one (see [Test.md](Test.md)).
* Fix every crash or error you hit before continuing. Report one unrelated to your change and record it in [TODO.md](TODO.md).
* Update the docs in the same change that alters the behavior they describe.

## Validation Gates

Run them in this fail-fast order, cheap deterministic checks first:

```bash
python3 dev/check_format.py     # formatting
python3 dev/ci_matrix.py        # ci.yml matches its generator
python3 build.py --framework glfw --config Release --clangd    # build and compile database
python3 dev/check_tidy.py       # clang-tidy
python3 dev/run_tests.py --framework <framework> --config Release    # test suite
```

* `check_format.py --fix` and `ci_matrix.py --fix` rewrite files in place; rerun the check-only commands for the gate.
* The suite tests an existing build: `glfw` on Windows and Linux, adding `--software` without a physical GPU; `macos` on macOS, built first with `python3 build.py --framework macos --config Release`.
* A focused `--case` run helps during development but does not replace the full suite when declaring a change complete.
* Requirements: `pip install clang-format==22.1.5 clang-tidy==22.1.7 autopep8` and Node.js (markdownlint runs via npx). clang-format majors 18 to 22 format this codebase identically; clang-tidy majors differ, and CI enforces 22.

## Coding Style

Follow the style already used in the codebase.

* **Modern C++20**: use current features (concepts, ranges, `std::format`, etc.).
* **Self-documenting code**: comment only structure, algorithms and non-obvious caveats. Comments state present facts, never history. If code needs a comment to be understood, reconsider the code first.
* **No dead code**: remove unused code, arguments, variables and includes immediately.
* **Simplicity**: prefer readability over cleverness; implement only what is needed, without speculative abstractions.
* **Don't repeat yourself**: reuse existing code; extract a function when a snippet appears twice; call a high-level wrapper rather than its internals.
* **Surgical changes**: every changed line traces to the task. Do not reformat code beyond your change.
* **Cross-platform paths**: use the `FileManager` and `Path` abstractions, never raw path separators.
* **RHI resources**: release GPU resources through the deferred deletion pattern.

### Formatting and Linting

Format with the tools, never by hand:

* C++, Objective-C++, Slang: clang-format with [.clang-format](../.clang-format).
* Python: autopep8, configured in [pyproject.toml](../pyproject.toml).
* Markdown: markdownlint with [.markdownlint.json](../.markdownlint.json).
* All C++ passes clang-tidy with [.clang-tidy](../.clang-tidy), every warning an error. Its naming rules in short:

| Construct                              | Style                        | Example                 |
| -------------------------------------- | ---------------------------- | ----------------------- |
| Namespace                              | `lower_case`                 | `sparkle`               |
| Class / Struct / Enum                  | `CamelCase`                  | `RenderFramework`       |
| Function                               | `CamelCase`                  | `BeginFrame()`          |
| Local variable, public member          | `lower_case`                 | `frame_count`           |
| Private / protected member             | `lower_case_` (trailing `_`) | `render_thread_`        |
| `constexpr` / static / global constant | `CamelCase`                  | `MaxBufferedTaskFrames` |
| Enum constant                          | `CamelCase`                  | `PrimaryLeft`           |

## Documentation

Docs are a user manual and a design doc, not a work log. Write for a user or developer meeting the project for the first time.

* Lead with intent and usage, then structure, then details. Leave implementation details to the code and link the module instead.
* State present-tense facts: knobs with defaults and tradeoffs, how to measure and debug, settled design constraints.
* Never narrate history: no change records, experiment stories or dates. Write negative results as present constraints ("X cannot help because Y").
* Never reference paths readers cannot see (gitignored or temporary files).
* Keep each topic in one file, listed in the [index](README.md), and link to it instead of repeating it.
* Agent-only procedures go in skills under `.agents/skills/`, indexed in [AGENTS.md](../AGENTS.md); skills link to docs and never copy them.
* Do not break lines inside a paragraph.
