---
name: verify-engine
description: 运行 Engine 的完整验证流程：构建 + headless 测试 + 注释审计。每次修复后必须执行。
---

# Engine verification protocol

Run these from `D:\dev\Engine`, in order. All must pass before a fix counts as done.

## 1. Build (zero new warnings)

```powershell
cmake --build build --parallel
```

If the cache is broken, reconfigure per CLAUDE.md, then rebuild.

## 2. Headless-safe tests

```powershell
ctest --test-dir build --output-on-failure -LE gpu
```

GPU-labelled tests (`engine_smoke`, some integration) need a real GL context —
run them only when a display is available; do not treat their absence as a pass
for renderer-behavior changes.

## 3. Targeted suites by change area

- ECS/world/serialization/render-prep/physics/Lua API (determinism-sensitive):
  ```powershell
  ctest --test-dir build --output-on-failure -R "determinism"
  ```
- Performance-sensitive (math, physics inner loops, command buffer):
  ```powershell
  cmake --build build --target engine_bench_ecs_perf engine_bench_physics_perf
  ctest --test-dir build --output-on-failure -R engine_bench_
  ```
  Compare against `tests/benchmark/perf_baseline.json`.

## 4. Comment audits

```powershell
python tools/check_source_comments.py
python tools/check_comment_quality.py --summary
```

Both audits must report ZERO findings (CI enforces this).

## 5. Bookkeeping

If the change affects module structure, build commands, test layout, or
roadmap status, update `CLAUDE.md` in the same commit.
New behavior requires a new/extended test per CLAUDE.md.
