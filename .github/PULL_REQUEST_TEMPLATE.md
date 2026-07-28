Please fill out the following template before submitting a pull request. Incomplete PRs may be delayed or rejected.

## Description

Provide a clear summary of what this PR changes and why:
- Describe the new feature, bug fix, or documentation update.
- Explain the motivation behind the change (e.g., addresses issue #XXX, improves performance for Y scenario).
- If applicable, include screenshots or example outputs that demonstrate the change.

## Related Issues

- Fixes: [#XXX](https://github.com/quintin-lee/clogx/issues/XXX)
- Addresses: [#XXX]
- Related: [#XXX]

## Type of Change

Select one:

- [ ] 🚀 **feat**: Adds a new feature
- [ ] 🐛 **fix**: Resolves a bug
- [ ] 📝 **docs**: Documentation only (README, user manual, etc.)
- [ ] ⚙️ **chore**: Build/tooling/update (no functional code change)
- [ ] ✨ **perf**: Performance improvement (no bug fix or new feature)
- [ ] 🧪 **test**: Adding or modifying tests only

## Testing Performed

Describe what tests were run and their results:

- [ ] `make check` passed with all warnings
- [ ] `make test` passed
- [ ] `make asan` passed (AddressSanitizer)
- [ ] `make ubsan` passed (UndefinedBehaviorSanitizer)
- [ ] `make test-valgrind` passed (Valgrind leak check)
- [ ] Fuzz tests ran without crashes (`make fuzz-config`, `make fuzz-formatter`)
- [ ] No new compiler warnings introduced

## Changelog Updated

- [ ] Yes — updated `CHANGELOG.md` under "Unreleased" section
- [ ] No — this change does not require a changelog entry (e.g., doc-only, trivial fix)

## Documentation Updated

- [ ] Yes — updated relevant documentation (user manual, README, API docs, etc.)
- [ ] No — documentation changes are not required for this change

## Screenshots / Examples (if applicable)

- [ ] Added screenshots showing UI/API changes
- [ ] No — this is a code-only change affecting logging behavior

## Review Checklist

- [ ] Code follows project style (run `make format` before committing)
- [ ] All new functions/modules have clear comments/docstrings
- [ ] Tests are adequate and cover the changed behavior
- [ ] No accidental additions (debug prints, large binary files, etc.)
- [ ] Commit messages follow [conventional commits](https://www.conventionalcommits.org/)
