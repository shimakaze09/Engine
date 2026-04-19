---
applyTo: "**/*.h,**/*.hpp,**/*.cpp,**/*.c,**/*.lua,**/CMakeLists.txt,tests/**"
---

# Karpathy Guidelines — LLM Coding Discipline

Behavioral constraints to prevent common LLM coding mistakes. These are hard gates on every code change.

## 1. Think Before Coding

- State assumptions explicitly before implementing. If uncertain, ask.
- If multiple valid interpretations exist, present them. Do not pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what is confusing. Ask.

## 2. Simplicity First

- No features beyond what was requested.
- No abstractions for single-use code.
- No speculative "flexibility" or "configurability."
- No error handling for impossible scenarios.
- If 200 lines can be 50, rewrite to 50.
- Test: "Would a senior engine programmer call this overcomplicated?" If yes, simplify.

## 3. Surgical Changes

- Touch only what the task requires.
- Do not "improve" adjacent code, comments, or formatting.
- Do not refactor things that are not broken.
- Match existing style, even if you would do it differently.
- If you notice unrelated dead code, mention it — do not delete it.
- Remove imports/variables/functions that YOUR changes made unused.
- Do not remove pre-existing dead code unless asked.
- Every changed line must trace directly to the user's request.

## 4. Goal-Driven Execution

- Transform every task into verifiable success criteria before starting.
- "Add validation" → "Write tests for invalid inputs, then make them pass."
- "Fix the bug" → "Write a test that reproduces it, then make it pass."
- "Refactor X" → "Ensure tests pass before and after."
- For multi-step tasks, state a brief plan with verification checks per step.
- Loop until success criteria are met. Do not declare done on assumption.
