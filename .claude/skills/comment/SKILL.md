---
name: comment
description: >
  Engine's commenting standard for C++ and CMake — what must carry a
  comment, what must not, the sanctioned markers, and the documentation
  expected of a public API. Load it before writing or editing comments,
  before adding a new source or header file, before documenting a public
  API in an include/ header, and when a comment audit reports a finding.
---

# Commenting standard

A comment describes the current design and why it must hold. It is part of
the code: wrong comments are defects, and a change that alters behavior
updates the comments that describe it.

Two mechanical gates back this up:
`tools/check_source_comments.py` (file-level comment presence) and
`tools/check_comment_quality.py` (tautologies, template stems, doc
comments misplaced above an access specifier or an init-list line,
commented-out code, untracked TODOs, issue numbers outside a tracked
marker). Both must report
zero findings. Everything else here is
judgement, checked in review.

## Required

**Every source, header, shader, build and test file** opens with a short
comment stating its role (the presence gate does not yet cover `.sc`
shaders, and most of them lack one) — what this translation unit is responsible for,
in its own terms. Not a restatement of the filename.

```cpp
// Owns the fixed-capacity slot tables that back every device resource
// handle, so a released handle is detected rather than aliasing a
// recycled backend object.
```

**Every public declaration in an `include/` header** documents what the
declaration is for, and whichever of these apply:

- **Ownership** — who owns the returned or stored resource, and who
  releases it.
- **Failure** — what happens on failure, and what the caller must check.
  Name the error type. If the function cannot fail, say so.
- **Threading** — which thread or phase may call it, and what it assumes
  about concurrent access. A real-time or hot-path API states its
  allocation behavior.
- **Units and frames** — seconds vs milliseconds, radians vs degrees,
  world vs local vs view space, row vs column major. A bare `float angle`
  is a defect waiting to happen.
- **Invariants** the caller must maintain or may rely on.

## Forbidden

- **Tautology.** `/// Handles the mesh.` above `handle_mesh()` adds
  nothing. Either say something the signature does not, or say nothing.
- **Templates.** Machine-shaped stems — "Stores X data used by the
  engine", "Owns the X behavior and state", "Returns the requested value"
  — are filler. The gate rejects them outright.
- **Commented-out code.** Delete it. Git holds it.
- **Doc comments in the wrong place** — directly above an access
  specifier or a constructor initializer line. Attach them to the
  declaration they describe.
- **Comments on private or self-evident declarations.** A private helper
  whose name and signature say everything carries no comment. Noise costs
  attention and drifts.
- **Change history.** A comment states what is true now, not what changed.
  "Formerly used the GL path", "as of 2026-03", "landed in the audit
  campaign" — all belong to git history, not to the code. The one
  exception is regression provenance in `tests/`, which is encouraged.

## Body comments

Reserve them for what the code cannot say: a non-obvious invariant, a
required ordering, a unit conversion, an ownership transfer, an external
constraint (a hardware limit, a format requirement, a library's
contract), or the reason a surprising construct is correct.

Explain **why**, never **what**:

```cpp
// bgfx rejects a stride override, so the vertex buffer is realized at its
// attachment's stride rather than the layout's.
```

Not:

```cpp
// Set the stride to the attachment stride.
```

If a body comment is explaining what the code does, the code needs a
better name or a smaller function instead.

## Markers

Six, and issue numbers appear in code only inside the first two
(plus regression provenance in tests). A comment names what the code does
and why it must hold; a reader with only the source cannot follow an
issue number, and the tracker already holds it. The gate rejects it:

| Marker | Use |
| --- | --- |
| `TODO(#n)` | Tracked, intended work. Never bare `TODO`. |
| `FIXME(#n)` | Tracked, known-wrong behavior. Never bare `FIXME`. |
| `WORKAROUND:` | Compensating for an external defect. Name the external thing and the condition that would let this go. |
| `HACK:` | Knowingly inelegant and load-bearing. Say what breaks if it is removed. |
| `NOTE:` | Non-obvious context a reader needs. |
| `WARNING:` | A trap: a call order, a lifetime, a reentrancy hazard. |

Tests carry one more, required by `tools/check_test_timing.py`:
`// wall-clock: harness-timeout` or `// wall-clock: diagnostic` on, or
within two lines above, every clock read in a functional test.

A bare `TODO` or `FIXME` is a gate finding. Either file an issue and cite
it, or do the work.

## Magic numbers and workarounds

A literal that is not self-evident gets a named constant, and the constant
gets the derivation:

```cpp
// DXBC exposes 16 sampler registers, and the shadow set consumes the
// four cascades plus the spot array, so the whole unit map must fit 15.
constexpr unsigned kMaxShadowSamplerUnits = 15U;
```

A workaround names the external cause, so a future reader can tell whether
it still applies.

## CMake

A `CMakeLists.txt` opens with what it builds. A dependency whose
visibility is not obvious carries the reason:

```cmake
# PRIVATE because bgfx is an implementation detail of the render device and
# must not propagate to targets that only consume the RenderDevice contract.
```

## Style

`///` for declaration documentation, `//` for body comments. Wrap at the
file's prevailing width. Match the surrounding code's density: a file that
documents every public declaration and nothing else stays that way.
