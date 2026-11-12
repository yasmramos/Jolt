<!--
PR title must follow Conventional Commits (CONVENTIONS.md §3):
  <type>(<scope>): <imperative summary, <= 72 chars, no trailing period>
Allowed types: feat, fix, docs, style, refactor, perf, test, build, ci, chore, revert.
Write everything in English. Do not add AI or tool attribution anywhere.
Delete the guidance comments and any section that does not apply.
-->

## What & why

<!--
One or two paragraphs: the problem and the reason for the change.
If behaviour changes, show it before/after with a table:

| Scenario | Expected | Before | After |
| --- | --- | --- | --- |
| ... | ... | ... | ... |
-->

## How

<!--
The implementation in bullets. Point at the files/functions that carry the
change. Explain the design choice, not the diff line by line.
-->

## What NOT to do (and why)

<!--
Oskel-specific guardrails (AGENT.md §3, §8). Delete if not relevant.
- Do **not** add a high-level opcode (`OP_IF`, `OP_EVAL_EXPR`, ...).
- Do **not** exceed the 40-opcode ceiling (`kOpcodeCount <= 40`).
- Do **not** repeat Java's mistake: bytecode that cannot be lowered to native.
- Do **not** add external dependencies beyond AsmJIT / ELFIO.
-->

## Checklist

### Language features only (AGENT.md §6)

<!-- Delete this block for changes that add no syntax. -->

- [ ] New lexer token? → which one
- [ ] New AST node? → which one
- [ ] New parser rule? → which one
- [ ] New bytecode opcode? → which one
- [ ] Opcode lowers to 1:1 or 1:2 ASM (or 1 runtime call)? → yes/no (redesign if 1:3+)
- [ ] New VM logic? → which one
- [ ] New compiler logic? → which one

### Architecture

- [ ] §2 toolchain boundary respected: `src/compiler.*` and `src/native/*` do not
      include `interpreter.h` or `vm.h` (`tests/check_compiler_boundary.cmake`)
- [ ] §3 bytecode contract unchanged, or the change is explained above
- [ ] §10 no side owners added to the cycle collector (`gc.cpp`)

## Tests

<!--
What was run and its result. Prefer concrete numbers.
- New/updated tests: which files and what they cover.
- `ctest`: N/N green.
- Manual checks: what was exercised.
- Any pre-existing failure that is unrelated (say how you confirmed it).
-->

## Notes

<!--
Docs updated (file + section). Migration notes. Follow-ups. Risk/rollback.
-->

- Docs updated:
- No new opcode / bytecode change / dependency: <!-- or explain -->
