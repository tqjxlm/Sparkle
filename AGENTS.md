# AGENTS.md

Guidance for AI coding agents. Keep it a thin router: documentation lives in [docs/](docs/README.md), task procedures in skills.

* [docs/README.md](docs/README.md) indexes the docs. Read the ones your task touches, not all of them.
* [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) applies to every change.
* Do not include yourself (the agent) in commit messages or authors.
* After pushing to a pull request, cancel its CI runs still in progress on older commits, unless an older commit's own result is wanted. The workflow does not cancel them itself, so a run can still tell whether a past commit is good.

## Skills

Task procedures are [Agent Skills](https://github.com/agentskills/agentskills) under `.agents/skills/<name>/SKILL.md`. Read the matching skill before starting such a task:

| skill                                                      | use when                                                      |
| ---------------------------------------------------------- | ------------------------------------------------------------- |
| [validate-change](.agents/skills/validate-change/SKILL.md) | before declaring a change finished, or when a reference fails |
| [debug-rendering](.agents/skills/debug-rendering/SKILL.md) | adding a rendering feature, or chasing an image artifact      |
