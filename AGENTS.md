# Repository instructions for agents

## Starting work

- In your first session in this repository, read `ARCHITECTURE.md` before changing code.
- If a plan is in progress, read its plan file as well. Check its status and completed steps before continuing work. Do not treat a completed plan as current work.

## Implementing a feature

1. Inspect the existing code and architecture. Prefer changes within current components and interfaces. Add an abstraction only when it removes a concrete duplication or solves a current need.
2. Before implementing a new feature, write a detailed plan that states the behavior, affected components, risks, and acceptance criteria. Divide it into small, ordered steps. For each step, name the tests that will gate completion.
3. Implement one step at a time with the smallest practical code change. Run its focused tests and `make test`; fix failures before starting the next step. Update the plan as steps finish or requirements change.
4. After all steps pass, update `ARCHITECTURE.md` if the system design changed. Delete a plan file once it is no longer needed; keep durable design facts in `ARCHITECTURE.md`.

## Testing strategy

- Test observable behavior and important boundaries, including failure and recovery paths where relevant.
- A test must depend on the behavior it claims to cover. If it passes with that behavior absent, it is a bad test.
- Check a new test by temporarily breaking the relevant implementation and running the focused test. The test should fail for the expected reason. Restore the implementation and confirm it passes. If a meaningful code change breaks no tests, add or strengthen coverage for that behavior.
- Keep tests focused and deterministic. Avoid assertions that merely repeat implementation details without checking a result.

## Commits

- Commit frequently at coherent, passing milestones. Follow `COMMIT_MESSAGES.md` for the subject and optional body.
- Include only related changes in each commit. Remove finished plan files when their contents are no longer needed.
