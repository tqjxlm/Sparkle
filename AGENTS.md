# AGENTS.md

This file provides guidance to AI coding agents when working with this repository. Keep it a thin router: put documentation in dedicated docs, not here.

* Read [README.md](README.md) for general background of this project and the location of other docs.
* Read other docs on demand:
  * [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md): coding style, tests, documentation rules, and validation gates. Apply it strictly.
  * [docs/Build.md](docs/Build.md): all build commands and build troubleshooting.
  * [docs/Run.md](docs/Run.md): how to launch the app, run arguments, log location, path conventions, etc..
  * [docs/Test.md](docs/Test.md): how to run tests, how to write tests, and how to test visual output. Screenshot capture mechanics live there too.
  * [docs/CI.md](docs/CI.md): CI/CD requirements and the local validation gates.
  * [docs/RenderingValidation.md](docs/RenderingValidation.md): the visual debugging methodology — semantic-first, perceptual salience over aggregate metrics, proxy-vs-ground-truth pitfalls. Read it before adding a rendering feature or chasing an image artifact.

## Agent Workflow

* Confirm a rendering defect with the user via a per-pixel visualization first, then gate the fix on that same agreed visualization — never on a quantitative metric alone. See [docs/RenderingValidation.md](docs/RenderingValidation.md).
* Leave heavy validation work to CI, don't try to run them all locally. But since not all tests are in CI, you still need to validate them locally if necessary. But avoid expensive ones like IBL cook parity, unless they are really relevant.
* Do not include yourself (the agent) in commit messages or authors.
