#!/usr/bin/env python3
"""Measure where cl.exe spends its time on the constructs that make a
handful of first-party translation units compile 10-40x slower under MSVC
than under clang-cl with identical flags.

Run from a Visual Studio developer environment (cl.exe and dumpbin.exe on
PATH), from the repository root. Every compile is sequential, so wall
times are not skewed by parallel jobs. Each compile records:

  wall      process wall time
  fe        front end (c1xx.dll) time, from /Bt+
  be        back end (c2.dll) time, from /Bt+
  obj       object file size in bytes
  text/data raw section sizes from dumpbin /headers

Stages:
  R  the real reproducer (tests/benchmark/ecs_perf_test.cpp)
  W  headers alone (world.h, physics_context.h, sparse_set.h), a slow real
     test that never constructs a World, and default member initializers
     that are declared but never used
  H  every first-party header compiled alone, ranked by front-end time
  F  candidate fixes on the isolated pattern: where the initializer lives,
     which constructor kind, and whether the unconstructed includer pays
  M  the real SparseSet reduced step by step (one construct removed each)
  I  an isolated copy of SparseSet with each member initializer switchable
  G  a grid of container x element type x initializer at one capacity
  S  capacity scaling for the pathological grid cells
  C  codegen inspection (/FAs listing statistics) for the worst cells
  A  equivalent storage representations: compile time, construction time
     and a semantic check of the constructed state

Results go to stdout, to $GITHUB_STEP_SUMMARY when set, and to
<out>/results.json; listings of stage C go to <out>/listings/.

Usage:
  python tools/ci/msvc_compile_probe.py --out probe-out [--stages RWHFMIGSCA]
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[2]

# The flags the CI lanes pass to first-party C++ (from the generated
# build.ninja), minus /WX so a synthetic warning cannot abort a sample.
BASE_FLAGS = [
    "/nologo", "/c", "/TP", "/std:c++latest", "/MD", "/DWIN32", "/D_WINDOWS",
    "/DNDEBUG", "/fp:strict", "/W4", "/permissive-", "/GR-", "/EHs-c-",
    "/wd4324", "/D_HAS_EXCEPTIONS=0", "/DENGINE_MAX_ENTITIES=65536",
    "/DENGINE_PLATFORM_WIN64=1",
]
OPT_FLAGS = {"O2": ["/O2", "/Ob2"], "Od": ["/Od", "/Ob0"]}
TIMING_FLAGS = ["/Bt+", "/d2cgsummary"]
MODULES = ("core", "math", "physics", "runtime", "content", "renderer",
           "scripting", "audio")
INCLUDES = [f"/I{REPO / m / 'include'}" for m in MODULES]
SRC_INCLUDES = [f"/I{REPO / m / 'src'}" for m in MODULES]
REPORT_RE = re.compile(r"^\s+(.+?):\s+([\d.]+)s\b")
TIMEOUT_S = 300

BT_RE = re.compile(r"time\(.*?(c1xx|c2)\.dll\)=([\d.]+)s", re.IGNORECASE)
SECTION_RE = re.compile(
    r"SECTION HEADER #\d+\s+(\S+) name.*?\s+([0-9A-F]+) size of raw data",
    re.DOTALL)


class Probe:
    def __init__(self, out: pathlib.Path) -> None:
        self.out = out
        self.work = out / "work"
        self.work.mkdir(parents=True, exist_ok=True)
        (out / "listings").mkdir(exist_ok=True)
        (out / "logs").mkdir(exist_ok=True)
        self.results: list[dict] = []
        self.lines: list[str] = []

    # ------------------------------------------------------------ plumbing
    def say(self, text: str = "") -> None:
        print(text, flush=True)
        self.lines.append(text)

    def compile(self, stage: str, name: str, source: str, opt: str,
                extra: list[str] | None = None, listing: bool = False,
                path: pathlib.Path | None = None, tag: str = "") -> dict:
        """Compiles one translation unit and returns its measurements."""
        if path is None:
            path = self.work / f"{name}.cpp"
            path.write_text(source, encoding="utf-8")
        obj = self.work / f"{name}.{opt}.obj"
        if obj.exists():
            obj.unlink()
        cmd = (["cl.exe"] + BASE_FLAGS + OPT_FLAGS[opt] + TIMING_FLAGS +
               INCLUDES + (extra or []) + [str(path), f"/Fo{obj}"])
        if listing:
            cmd.append(f"/Fa{self.out / 'listings' / f'{name}.{opt}.asm'}")
            cmd.append("/FAs")
        start = time.perf_counter()
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=TIMEOUT_S, cwd=self.work)
            output = proc.stdout + proc.stderr
            rc = proc.returncode
        except subprocess.TimeoutExpired as exc:
            output = (exc.stdout or "") if isinstance(exc.stdout, str) else ""
            rc = "timeout"
        wall = time.perf_counter() - start
        fe = be = None
        for which, seconds in BT_RE.findall(output):
            if which.lower() == "c1xx":
                fe = float(seconds)
            else:
                be = float(seconds)
        record = {
            "stage": stage, "name": name, "opt": opt, "rc": rc,
            "wall": round(wall, 3), "fe": fe, "be": be,
            "obj": obj.stat().st_size if obj.exists() else None,
        }
        if obj.exists():
            record.update(self.sections(obj))
        summary = self.cg_summary(output)
        if summary:
            record["cgsummary"] = summary
        if rc not in (0, "timeout"):
            record["error"] = output[-1500:]
        # Keep the compiler's own report for anything slow or instrumented.
        if tag or wall > 2.0 or rc != 0:
            log = self.out / "logs" / f"{name}.{opt}{tag}.txt"
            log.write_text(output, encoding="utf-8")
        self.results.append(record)
        self.say(self.row(record))
        return record

    @staticmethod
    def sections(obj: pathlib.Path) -> dict:
        """Raw sizes of the object's .text, .data, .rdata and .bss."""
        try:
            text = subprocess.run(["dumpbin.exe", "/headers", str(obj)],
                                  capture_output=True, text=True,
                                  timeout=120).stdout
        except (OSError, subprocess.TimeoutExpired):
            return {}
        sizes: dict = {}
        for name, raw in SECTION_RE.findall(text):
            key = name.split("$")[0].lstrip(".")
            if key in ("text", "data", "rdata", "bss"):
                sizes[key] = sizes.get(key, 0) + int(raw, 16)
        return sizes

    @staticmethod
    def cg_summary(output: str) -> list[str]:
        """The /d2cgsummary block: totals plus anomalous functions."""
        lines = output.splitlines()
        for i, line in enumerate(lines):
            if "Code Generation Summary" in line:
                block = [l.strip() for l in lines[i + 1:i + 40] if l.strip()]
                return block[:25]
        return []

    @staticmethod
    def row(r: dict) -> str:
        def f(v):
            return "-" if v is None else f"{v:.2f}"
        extra = ""
        if r.get("text") is not None:
            extra = (f" text={r.get('text', 0)} data={r.get('data', 0)}"
                     f" rdata={r.get('rdata', 0)} bss={r.get('bss', 0)}")
        return (f"  {r['stage']} {r['name']:<44} {r['opt']:<3} rc={r['rc']!s:<7}"
                f" wall={f(r['wall'])} fe={f(r['fe'])} be={f(r['be'])}"
                f" obj={r['obj']}{extra}")

    def version(self) -> None:
        proc = subprocess.run(["cl.exe"], capture_output=True, text=True)
        banner = (proc.stderr or proc.stdout).splitlines()
        self.say("cl.exe: " + (banner[0] if banner else "unknown"))

    # ------------------------------------------------------------- stage R
    def stage_r(self) -> None:
        self.say("\n## R: the real reproducer")
        src = REPO / "tests/benchmark/ecs_perf_test.cpp"
        for opt in ("O2", "Od"):
            self.compile("R", "ecs_perf_test", "", opt, path=src)
        # Front-end detail: time per include, class and function.
        self.compile("R", "ecs_perf_test", "", "O2", path=src,
                     extra=["/d1reportTime"], tag=".reportTime")
        self.report_top("ecs_perf_test", "O2", ".reportTime")

    def report_top(self, name: str, opt: str, tag: str, count: int = 25) -> None:
        """Prints the slowest entries of a saved /d1reportTime log, each
        with the report section it belongs to."""
        log = self.out / "logs" / f"{name}.{opt}{tag}.txt"
        if not log.exists():
            return
        section = ""
        entries = []
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            if line and not line[0].isspace() and line.rstrip().endswith(":"):
                section = line.strip().rstrip(":")
                continue
            m = REPORT_RE.match(line)
            if m:
                entries.append((float(m.group(2)), section, m.group(1).strip()))
        entries.sort(reverse=True)
        self.say(f"    /d1reportTime top {count} for {name}:")
        for seconds, sec, what in entries[:count]:
            self.say(f"      {seconds:8.3f}s  [{sec}] {what[:150]}")

    # ------------------------------------------------------------- stage W
    DECL_HEAD = """\
#include <array>
#include <cstddef>
constexpr std::size_t N = 65536;
struct V { float x; float y; float z;
  constexpr V() noexcept : x(0.0F), y(0.0F), z(0.0F) {} };
struct T { V position = V(); };
"""

    def stage_w(self) -> None:
        self.say("\n## W: headers and never-used default member initializers")
        headers = {
            "w1_world_h": '#include "engine/runtime/world.h"\n',
            "w2_physics_context_h": '#include "engine/physics/physics_context.h"\n',
            "w3_sparse_set_h": '#include "engine/core/sparse_set.h"\n',
        }
        for name, src in headers.items():
            for opt in ("O2", "Od"):
                self.compile("W", name, src, opt)
        self.compile("W", "w1_world_h", headers["w1_world_h"], "O2",
                     extra=["/d1reportTime"], tag=".reportTime")
        self.report_top("w1_world_h", "O2", ".reportTime")
        decls = {
            # Non-template class: the initializer is analysed where the
            # class is defined, whether or not anything constructs it.
            "w4_decl_only_braced": "struct S { std::array<T, N> a{}; S() noexcept; };\n",
            "w5_decl_only_none": "struct S { std::array<T, N> a; S() noexcept; };\n",
            "w6_decl_only_int_braced": "struct S { std::array<int, N> a{}; S() noexcept; };\n",
            # Class template instantiated as a member of a non-template
            # class that is itself never constructed.
            "w7_template_member_braced": ("template <int K> struct S { std::array<T, N> a{};"
                                          " S() noexcept; };\n"
                                          "struct U { S<0> s; U() noexcept; };\n"),
            # Ten braced members in one non-template class (World's shape).
            "w8_decl_only_ten_braced": ("struct S {\n" + "".join(
                f"  std::array<T, N> a{i}{{}};\n" for i in range(10)) +
                "  S() noexcept; };\n"),
        }
        for name, body in decls.items():
            for opt in ("O2", "Od"):
                self.compile("W", name, self.DECL_HEAD + body, opt)
        slow_test = REPO / "tests/unit/world_name_lookup_test.cpp"
        for opt in ("O2", "Od"):
            self.compile("W", "world_name_lookup_test", "", opt, path=slow_test)

    # ------------------------------------------------------------- stage H
    def stage_h(self) -> None:
        self.say("\n## H: every first-party header alone (/O2), slowest first")
        rows = []
        for module in MODULES:
            for header in sorted((REPO / module).glob("**/*.h")):
                rel = header.relative_to(REPO).as_posix()
                name = "h_" + re.sub(r"[^A-Za-z0-9]+", "_", rel)
                src = f'#include "{header.as_posix()}"\n'
                path = self.work / f"{name}.cpp"
                path.write_text(src, encoding="utf-8")
                cmd = (["cl.exe"] + BASE_FLAGS + OPT_FLAGS["O2"] + ["/Bt+"] +
                       INCLUDES + SRC_INCLUDES + [str(path), f"/Fo{self.work / name}.obj"])
                try:
                    proc = subprocess.run(cmd, capture_output=True, text=True,
                                          timeout=TIMEOUT_S, cwd=self.work)
                    out = proc.stdout + proc.stderr
                    fe = next((float(t) for w, t in BT_RE.findall(out)
                               if w.lower() == "c1xx"), None)
                    rc = proc.returncode
                except subprocess.TimeoutExpired:
                    fe, rc = None, "timeout"
                rows.append((fe if fe is not None else -1.0, rc, rel))
                self.results.append({"stage": "H", "name": rel, "rc": rc, "fe": fe})
        rows.sort(reverse=True)
        for fe, rc, rel in rows[:40]:
            self.say(f"  H {fe:7.2f}s rc={rc!s:<3} {rel}")
        failed = [rel for fe, rc, rel in rows if rc != 0]
        self.say(f"  H {len(rows)} headers, {len(failed)} did not compile alone")

    # ------------------------------------------------------------- stage F
    # T is the benchmark's element shape; N the World's entity capacity.
    FIX_HEAD = """\
#include <array>
#include <cstddef>
constexpr std::size_t N = 65536;
struct V { float x; float y; float z;
  constexpr V() noexcept : x(0.0F), y(0.0F), z(0.0F) {} };
struct T { V position = V(); unsigned links = 0U; };
template <typename E, std::size_t K> struct ImplicitCtorSet {
  std::array<E, K> a{};
};
template <typename E, std::size_t K> struct UserCtorSet {
  UserCtorSet() noexcept {}
  std::array<E, K> a{};
};
template <typename E, std::size_t K> struct ValueArray : std::array<E, K> {
  ValueArray() noexcept : std::array<E, K>{} {}
};
"""
    FIXES = {
        # Today's form: braced default member initializer in a plain class,
        # included, never constructed.
        "f0_nsdmi_braced": "struct S { std::array<T, N> a{}; S() noexcept; };\n",
        # Initializer moved to the out-of-line constructor's mem-init list:
        # the includer sees no initializer at all.
        "f1_meminit_includer": "struct S { std::array<T, N> a; S() noexcept; };\n",
        # The one translation unit that defines that constructor.
        "f2_meminit_definer": ("struct S { std::array<T, N> a; S() noexcept; };\n"
                               "S::S() noexcept : a{} {}\n"),
        # Class template with an implicit constructor, value-initialized by a
        # plain class's braced member (CompactSparseSet in World).
        "f3_template_implicit_ctor": ("struct S { ImplicitCtorSet<T, N> s{}; S() noexcept; };\n"),
        # The same template with a user-provided constructor (SparseSet).
        "f4_template_user_ctor": ("struct S { UserCtorSet<T, N> s{}; S() noexcept; };\n"),
        # A value-initializing array wrapper with a user-provided constructor.
        "f5_wrapper_braced": "struct S { ValueArray<T, N> a{}; S() noexcept; };\n",
        "f6_wrapper_definer": ("struct S { ValueArray<T, N> a{}; S() noexcept; };\n"
                               "S::S() noexcept {}\n"),
    }

    def stage_f(self) -> None:
        self.say("\n## F: candidate fixes (N=65536, element with initializers)")
        for name, body in self.FIXES.items():
            for opt in ("O2", "Od"):
                self.compile("F", name, self.FIX_HEAD + body, opt)

    # ------------------------------------------------------------- stage M
    MINI_HEAD = """\
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include "engine/core/sparse_set.h"
#include "engine/math/vec3.h"
struct BenchEntity final { std::uint32_t index = 0U; };
struct BenchTransform final {
  engine::math::Vec3 position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
};
struct PodComponent final { float x; float y; float z; };
struct PodEntity final { std::uint32_t index; };
"""

    def stage_m(self) -> None:
        self.say("\n## M: ecs_perf_test reduced to its SparseSet use")
        variants = {
            # The benchmark's two sets, constructed as the benchmark does.
            "m1_construct_bench_types": """
using Set = engine::core::SparseSet<BenchEntity, BenchTransform, 50000, 50000>;
Set *make() { return new (std::nothrow) Set(); }""",
            # The same type, never constructed: only member functions used.
            "m2_no_construct": """
using Set = engine::core::SparseSet<BenchEntity, BenchTransform, 50000, 50000>;
bool touch(Set &s) { return s.add(BenchEntity{1U}, BenchTransform{}); }""",
            "m3_component_int": """
using Set = engine::core::SparseSet<BenchEntity, int, 50000, 50000>;
Set *make() { return new (std::nothrow) Set(); }""",
            "m4_component_pod": """
using Set = engine::core::SparseSet<BenchEntity, PodComponent, 50000, 50000>;
Set *make() { return new (std::nothrow) Set(); }""",
            "m5_entity_pod": """
using Set = engine::core::SparseSet<PodEntity, BenchTransform, 50000, 50000>;
Set *make() { return new (std::nothrow) Set(); }""",
            "m6_both_pod": """
using Set = engine::core::SparseSet<PodEntity, PodComponent, 50000, 50000>;
Set *make() { return new (std::nothrow) Set(); }""",
            "m7_capacity_4096": """
using Set = engine::core::SparseSet<BenchEntity, BenchTransform, 4096, 4096>;
Set *make() { return new (std::nothrow) Set(); }""",
        }
        for name, body in variants.items():
            for opt in ("O2", "Od"):
                self.compile("M", name, self.MINI_HEAD + body, opt)

    # ------------------------------------------------------------- stage I
    # SparseSet's storage members, each initializer switchable, with the
    # element types the benchmark uses. INIT_C/INIT_E/INIT_S are "{}" or "".
    ISO = """\
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
struct Vec3 { float x; float y; float z;
  constexpr Vec3() noexcept : x(0.0F), y(0.0F), z(0.0F) {}
  constexpr Vec3(float a, float b, float c) noexcept : x(a), y(b), z(c) {} };
struct Entity { std::uint32_t index = 0U; };
struct Transform { Vec3 position = Vec3(0.0F, 0.0F, 0.0F); };
template <typename E, typename C, std::size_t N> class Set {
public:
  Set() noexcept { CTOR_BODY }
  std::array<std::array<C, N>, 1> m_components INIT_C;
  std::array<E, N> m_entities INIT_E;
  std::array<std::int32_t, N + 1> m_sparse INIT_S;
  std::size_t m_count = 0U;
};
using S = Set<Entity, Transform, 50000>;
S *make() { return new (std::nothrow) S(); }
"""

    def stage_i(self) -> None:
        self.say("\n## I: isolated SparseSet members (N=50000)")
        fill = "m_sparse.fill(-1);"
        cases = [
            ("i0_all_braced_fill", "{}", "{}", "{}", fill),
            ("i1_components_only", "{}", "", "", fill),
            ("i2_entities_only", "", "{}", "", fill),
            ("i3_sparse_only", "", "", "{}", fill),
            ("i4_none_braced", "", "", "", fill),
            ("i5_all_braced_empty_ctor", "{}", "{}", "{}", ""),
        ]
        for name, c, e, s, body in cases:
            src = (self.ISO.replace("INIT_C", c).replace("INIT_E", e)
                   .replace("INIT_S", s).replace("CTOR_BODY", body))
            for opt in ("O2", "Od"):
                self.compile("I", name, src, opt)

    # ------------------------------------------------------- stages G/S/C
    ELEMENTS = {
        "int": "using T = int;",
        "pod": "struct T { float x; float y; float z; };",
        "nsdmi0": "struct T { float x = 0.0F; float y = 0.0F; float z = 0.0F; };",
        "nsdmi1": "struct T { float x = 1.0F; float y = 1.0F; float z = 1.0F; };",
        "ctor": ("struct T { float x; float y; float z; constexpr T() noexcept"
                 " : x(0.0F), y(0.0F), z(0.0F) {} };"),
        "holds_ctor": ("struct V { float x; float y; float z; constexpr V()"
                       " noexcept : x(0.0F), y(0.0F), z(0.0F) {} };"
                       " struct T { V position = V(); };"),
    }
    CONTAINERS = {
        # Member array, constructor defined out of line (World's shape).
        "member_ooctor": """struct S { std::array<T, N> a INIT; S() noexcept; };
S::S() noexcept {}
S *make() { return new (std::nothrow) S(); }""",
        # Member array, inline user-provided constructor (SparseSet's shape).
        "member_inline_ctor": """struct S { std::array<T, N> a INIT; S() noexcept {} };
S *make() { return new (std::nothrow) S(); }""",
        # Member array, implicit constructor, value-initialized by new S().
        "member_implicit_value": """struct S { std::array<T, N> a INIT; };
S *make() { return new (std::nothrow) S(); }""",
        # Member array, implicit constructor, default-initialized by new S.
        "member_implicit_default": """struct S { std::array<T, N> a INIT; };
S *make() { return new (std::nothrow) S; }""",
        # Raw array member, inline constructor.
        "member_raw": """struct S { T a[N] INIT; S() noexcept {} };
S *make() { return new (std::nothrow) S(); }""",
        # Namespace-scope array (the console ring's shape).
        "global": """std::array<T, N> g INIT;
T *get() { return g.data(); }""",
        # Function-local static.
        "static_local": """T *get() { static std::array<T, N> s INIT; return s.data(); }""",
        # Automatic storage.
        "stack_local": """void sink(T *) noexcept;
void f() { std::array<T, N> a INIT; sink(a.data()); }""",
    }

    def grid_source(self, container: str, element: str, init: str,
                    n: int) -> str:
        return ("#include <array>\n#include <cstddef>\n#include <new>\n"
                f"constexpr std::size_t N = {n};\n"
                + self.ELEMENTS[element] + "\n"
                + self.CONTAINERS[container].replace("INIT", init) + "\n")

    def stage_g(self, n: int = 65536) -> list[dict]:
        self.say(f"\n## G: container x element x initializer (N={n})")
        rows = []
        for container in self.CONTAINERS:
            for element in self.ELEMENTS:
                for init_name, init in (("brace", "{}"), ("none", "")):
                    name = f"g_{container}_{element}_{init_name}"
                    src = self.grid_source(container, element, init, n)
                    for opt in ("O2", "Od"):
                        r = self.compile("G", name, src, opt)
                        r.update(container=container, element=element,
                                 init=init_name)
                        rows.append(r)
        return rows

    def stage_s(self, grid: list[dict]) -> list[tuple]:
        self.say("\n## S: capacity scaling for the slowest grid cells")
        def cost(r):
            return (r["fe"] or 0.0) + (r["be"] or 0.0) if r["rc"] == 0 else 1e9
        slow = sorted((r for r in grid if r["opt"] == "O2"), key=cost,
                      reverse=True)
        picks = []
        for r in slow:
            key = (r["container"], r["element"], r["init"])
            if key not in picks and cost(r) > 1.0:
                picks.append(key)
            if len(picks) == 4:
                break
        # Always scale the benchmark's own shape as the reference cell.
        ref = ("member_inline_ctor", "holds_ctor", "brace")
        if ref not in picks:
            picks.append(ref)
        for container, element, init_name in picks:
            init = "{}" if init_name == "brace" else ""
            for n in (256, 1024, 4096, 16384, 65536, 262144):
                name = f"s_{container}_{element}_{init_name}_{n}"
                src = self.grid_source(container, element, init, n)
                for opt in ("O2", "Od"):
                    r = self.compile("S", name, src, opt)
                    r.update(container=container, element=element,
                             init=init_name, n=n)
        return picks

    def stage_c(self, picks: list[tuple]) -> None:
        self.say("\n## C: generated code for the slowest cells")
        for container, element, init_name in picks[:3]:
            init = "{}" if init_name == "brace" else ""
            for n in (4096, 65536):
                name = f"c_{container}_{element}_{init_name}_{n}"
                src = self.grid_source(container, element, init, n)
                for opt in ("O2", "Od"):
                    self.compile("C", name, src, opt, listing=True)
                    self.listing_stats(name, opt)

    def listing_stats(self, name: str, opt: str) -> None:
        asm = self.out / "listings" / f"{name}.{opt}.asm"
        if not asm.exists():
            self.say(f"    listing missing for {name} {opt}")
            return
        text = asm.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        stats = {
            "lines": len(lines),
            "memset_calls": len(re.findall(r"call\s+\S*memset", text)),
            "rep_stos": len(re.findall(r"\brep\s+stos", text)),
            "store_movs": len(re.findall(r"\bmov(?:ss|ups|aps|dqu|q|d)?\s+"
                                         r"(?:DWORD|QWORD|XMMWORD)?\s*PTR", text)),
            "backward_jumps": len(re.findall(r"\bj(?:ne|b|l|nz|ae|g)\s+SHORT\s+\$LL",
                                             text)) +
                              len(re.findall(r"\$LL\d+@", text)),
            "vector_ctor_iterator": len(re.findall(r"vector constructor iterator",
                                                   text)),
            "dynamic_initializer": len(re.findall(r"dynamic initializer", text)),
        }
        self.results.append({"stage": "C-listing", "name": name, "opt": opt,
                             **stats})
        self.say(f"    listing {name} {opt}: " +
                 " ".join(f"{k}={v}" for k, v in stats.items()))
        # Keep the head of big listings readable in the artifact.
        if len(lines) > 4000:
            head = "\n".join(lines[:2000] + ["; ... truncated ..."] + lines[-500:])
            asm.write_text(head, encoding="utf-8")

    # ------------------------------------------------------------- stage A
    # Equivalent storage representations for SparseSet's component array.
    # Each builds as an executable that constructs the set 20 times and
    # checks the constructed component storage equals value-initialization.
    ALT_HEAD = """\
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
struct Vec3 { float x; float y; float z;
  constexpr Vec3() noexcept : x(0.0F), y(0.0F), z(0.0F) {}
  constexpr Vec3(float a, float b, float c) noexcept : x(a), y(b), z(c) {} };
struct Transform { Vec3 position = Vec3(0.0F, 0.0F, 0.0F); };
constexpr std::size_t N = 50000;
"""
    ALT_MAIN = """
int main() {
  double best = 1e30;
  bool ok = true;
  for (int round = 0; round < 20; ++round) {
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<S> s(new (std::nothrow) S());
    const auto t1 = std::chrono::steady_clock::now();
    if (!s) return 2;
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    if (us < best) best = us;
    const Transform expected{};
    for (std::size_t i = 0; i < N; ++i) {
      if (std::memcmp(&s->at(i), &expected, sizeof(Transform)) != 0) ok = false;
    }
  }
  std::printf("construct_us=%.1f value_initialized=%s\\n", best, ok ? "yes" : "NO");
  return ok ? 0 : 1;
}
"""
    ALTERNATIVES = {
        "a0_std_array_braced": """struct S { std::array<Transform, N> a{};
  S() noexcept {} Transform &at(std::size_t i) { return a[i]; } };""",
        "a1_std_array_default_init": """struct S { std::array<Transform, N> a;
  S() noexcept {} Transform &at(std::size_t i) { return a[i]; } };""",
        "a2_raw_array_braced": """struct S { Transform a[N]{};
  S() noexcept {} Transform &at(std::size_t i) { return a[i]; } };""",
        "a3_raw_array_default_init": """struct S { Transform a[N];
  S() noexcept {} Transform &at(std::size_t i) { return a[i]; } };""",
        "a4_std_array_fill_in_ctor": """struct S { std::array<Transform, N> a;
  S() noexcept { a.fill(Transform{}); } Transform &at(std::size_t i) { return a[i]; } };""",
        "a5_raw_bytes_zeroed": """static_assert(std::is_trivially_copyable_v<Transform>);
struct S { alignas(Transform) unsigned char bytes[sizeof(Transform) * N];
  S() noexcept { std::memset(bytes, 0, sizeof(bytes)); }
  Transform &at(std::size_t i) { return *std::launder(reinterpret_cast<Transform *>(bytes) + i); } };""",
        "a6_heap_array": """struct S { std::unique_ptr<Transform[]> a{new (std::nothrow) Transform[N]()};
  S() noexcept {} Transform &at(std::size_t i) { return a[i]; } };""",
    }

    def stage_a(self) -> None:
        self.say("\n## A: equivalent storage representations (N=50000)")
        for name, body in self.ALTERNATIVES.items():
            src = self.ALT_HEAD + body + self.ALT_MAIN
            path = self.work / f"{name}.cpp"
            path.write_text(src, encoding="utf-8")
            for opt in ("O2", "Od"):
                self.compile("A", name, src, opt, path=path)
            exe = self.work / f"{name}.exe"
            link = subprocess.run(
                ["cl.exe", "/nologo", "/std:c++latest", "/MD", "/O2", "/Ob2",
                 "/DNDEBUG", "/fp:strict", "/GR-", "/EHs-c-",
                 "/D_HAS_EXCEPTIONS=0", str(path), f"/Fe{exe}",
                 f"/Fo{self.work / (name + '.link.obj')}"],
                capture_output=True, text=True, timeout=TIMEOUT_S,
                cwd=self.work)
            if link.returncode != 0 or not exe.exists():
                self.say(f"    run {name}: build failed")
                continue
            run = subprocess.run([str(exe)], capture_output=True, text=True,
                                 timeout=120)
            self.say(f"    run {name}: {run.stdout.strip()} rc={run.returncode}")
            self.results.append({"stage": "A-run", "name": name,
                                 "output": run.stdout.strip(),
                                 "rc": run.returncode})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default="probe-out")
    parser.add_argument("--stages", default="RWHFMIGSCA")
    args = parser.parse_args()
    probe = Probe(pathlib.Path(args.out).resolve())
    probe.version()
    stages = args.stages.upper()
    if "R" in stages:
        probe.stage_r()
    if "W" in stages:
        probe.stage_w()
    if "F" in stages:
        probe.stage_f()
    if "H" in stages:
        probe.stage_h()
    if "M" in stages:
        probe.stage_m()
    if "I" in stages:
        probe.stage_i()
    picks: list[tuple] = []
    if "G" in stages:
        grid = probe.stage_g()
        if "S" in stages:
            picks = probe.stage_s(grid)
    if "C" in stages and picks:
        probe.stage_c(picks)
    if "A" in stages:
        probe.stage_a()
    (probe.out / "results.json").write_text(
        json.dumps(probe.results, indent=1), encoding="utf-8")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as handle:
            handle.write("```\n" + "\n".join(probe.lines) + "\n```\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
