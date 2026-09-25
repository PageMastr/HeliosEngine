# Contributing to Helios MMO Engine

Helios is released under the [MIT License](LICENSE). By contributing, you agree that your contribution is
licensed under the same terms.

The full contribution guide lives in the README:

- [Contributing](README.md#contributing): finding work, forking and branching, commits, pull requests,
  code requirements, legal/IP rules, security reports.
- [AI contributions and rules for agents](README.md#ai-contributions-and-rules-for-agents): disclosure,
  accountability, and the rules every AI agent must follow.

Binding engineering conventions (platforms, build commands, layering, style, tests) are in
[`CLAUDE.md`](CLAUDE.md). Architecture decisions are in
[`docs/plan/00-decisions.md`](docs/plan/00-decisions.md).

Quick checklist before opening a pull request:

- [ ] Branched from the latest `main`; one logical change; branch named `feat/`, `fix/`, `perf/`, `docs/`,
      `test/`, `ci/`, `chore/` or `agent/<tool>/`.
- [ ] Builds warning-clean and tests pass locally (and on every CI job).
- [ ] New behaviour has tests; timing gates are named `perf: ...`.
- [ ] Plan/ADR updated if the design changed.
- [ ] No secrets, build output, downloaded binaries or third-party IP committed.
- [ ] AI involvement disclosed in the PR template.
