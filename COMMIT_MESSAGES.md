# Commit messages

Follow the style used in this repository's history:

```text
type: concise action in lowercase

Optional body explaining why the change was needed and any behavior that is
not clear from the subject.
```

- Use a lowercase type followed by a colon and one space. Common types here are `feat`, `fix`, `docs`, `refactor`, `style`, and `chore`. Use `test` for a tests-only change.
- State what the commit does with an action verb. Keep the subject specific and brief; omit a trailing period.
- Describe the whole commit in the subject. When code, tests, and documentation serve one change, use the type for the main purpose.
- Add a body for changes that need context, such as a failure mode, compatibility decision, or non-obvious test setup. Separate it from the subject with a blank line. The body is optional for straightforward commits.

Examples from the history: `feat: reclaim obsolete versions with snapshots`, `fix: edge cases in memory allocator`, and `docs: describe compaction design and remaining snapshot work`.
