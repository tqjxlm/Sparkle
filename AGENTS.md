# AGENTS.md

Guidance for AI coding agents. Keep it a thin router: documentation lives in [docs/](docs/README.md), task procedures in skills.

* [docs/README.md](docs/README.md) indexes the docs. Read the ones your task touches, not all of them.
* [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) applies to every change.
* Do not include yourself (the agent) in commit messages or authors.

## Skills

Task procedures are [Agent Skills](https://github.com/agentskills/agentskills) under `.agents/skills/<name>/SKILL.md`. Read the matching skill before starting such a task:

| skill                                                      | use when                                                      |
| ---------------------------------------------------------- | ------------------------------------------------------------- |
| [validate-change](.agents/skills/validate-change/SKILL.md) | before declaring a change finished, or when a reference fails |
| [debug-rendering](.agents/skills/debug-rendering/SKILL.md) | adding a rendering feature, or chasing an image artifact      |
