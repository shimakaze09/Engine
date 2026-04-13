---
description: 'Systematic diagnosis of an engine defect: crash, logic error, or regression'
---
Diagnose and fix the following defect: ${input:bugDescription}

## Diagnosis Protocol

### Step 1: Reproduce
1. Identify the minimal reproduction path.
2. Determine which module(s) are involved.
3. Check if the issue is phase-dependent (WorldPhase contract violation).

### Step 2: Isolate
1. Read the relevant source files. Do not guess — read the code.
2. Trace the execution path from entry point to failure.
3. Check for these defect classes (in order of likelihood):
   - Phase contract violation (mutation outside correct WorldPhase)
   - Null dereference (missing g_world/g_services check)
   - Buffer overflow (SparseSet capacity, fixed array bounds)
   - Use-after-free (stale entity handle, generation mismatch)
   - Resource leak (GPU handle, Lua reference, file handle)
   - Hot-path allocation (std::vector/std::string in per-frame code)
   - Module boundary violation (upward dependency)
   - Silent failure (missing error log on failure path)

### Step 3: Fix
1. Write the minimal fix. Do not refactor unrelated code.
2. Verify zero warnings on the changed files.
3. Add a regression test that fails before the fix and passes after.
4. Verify no existing tests break.

### Step 4: Verify
```powershell
cmake --build build
ctest --test-dir build --output-on-failure
```

## Output

1. **Root cause**: one sentence.
2. **Fix**: exact code change with file paths.
3. **Regression test**: test function that prevents recurrence.
4. **Risk assessment**: what other code paths could have the same class of bug.
