#!/usr/bin/env python3
"""Lua binding generator for the engine.

Scans C++ header files for // LUA_BIND: annotations and generates
Lua C API wrapper functions.

Annotation syntax (placed immediately above a function declaration):
    // LUA_BIND: <lua_name>(<param>: <type>, ...) -> <return_type>

Supported parameter types:
    int       - lua_Integer (luaL_checkinteger)
    float     - lua_Number  (luaL_checknumber, cast to float)
    double    - lua_Number  (luaL_checknumber)
    bool      - int         (lua_toboolean)
    string    - const char* (luaL_checkstring)
    entity    - uint32_t    (luaL_checkinteger, entity index)
    uint32    - uint32_t    (luaL_checkinteger)

Supported return types:
    void      - no return value
    int       - lua_pushinteger
    float     - lua_pushnumber
    double    - lua_pushnumber
    bool      - lua_pushboolean
    string    - lua_pushstring
    entity    - lua_pushinteger (entity index)
    uint32    - lua_pushinteger

Usage:
    python generate_bindings.py <input_header> [<input_header2> ...] -o <output.cpp>
"""

import re
import sys
import argparse
from pathlib import Path

# Regex for the annotation line.
BIND_RE = re.compile(
    r'//\s*LUA_BIND:\s*(\w+)'           # lua_name
    r'\(([^)]*)\)'                       # params (may be empty)
    r'\s*->\s*(\w+)'                     # return type
)

# Regex for a C++ function declaration following the annotation.
FUNC_RE = re.compile(
    r'[\w:*&\s]+\b(\w+)\s*\([^)]*\)'    # function name
)

# Type mapping: annotation type -> (C++ type, Lua check expr, Lua push expr)
TYPE_MAP = {
    'int':    ('lua_Integer',  'luaL_checkinteger(L, {i})',     'lua_pushinteger(L, result)'),
    'float':  ('float',        'static_cast<float>(luaL_checknumber(L, {i}))',
                                                                 'lua_pushnumber(L, static_cast<lua_Number>(result))'),
    'double': ('lua_Number',   'luaL_checknumber(L, {i})',      'lua_pushnumber(L, result)'),
    'bool':   ('bool',         'lua_toboolean(L, {i}) != 0',    'lua_pushboolean(L, result ? 1 : 0)'),
    'string': ('const char*',  'luaL_checkstring(L, {i})',      'lua_pushstring(L, result)'),
    'entity': ('std::uint32_t',
               'static_cast<std::uint32_t>(luaL_checkinteger(L, {i}))',
               'lua_pushinteger(L, static_cast<lua_Integer>(result))'),
    'uint32': ('std::uint32_t',
               'static_cast<std::uint32_t>(luaL_checkinteger(L, {i}))',
               'lua_pushinteger(L, static_cast<lua_Integer>(result))'),
}


def parse_params(param_str):
    """Parse 'name: type, name2: type2' into list of (name, type_key)."""
    params = []
    param_str = param_str.strip()
    if not param_str:
        return params
    for part in param_str.split(','):
        part = part.strip()
        if ':' not in part:
            raise ValueError(f"Bad parameter format: '{part}' (expected 'name: type')")
        name, typ = part.split(':', 1)
        name = name.strip()
        typ = typ.strip()
        if typ not in TYPE_MAP:
            raise ValueError(f"Unknown type '{typ}' for parameter '{name}'")
        params.append((name, typ))
    return params


def parse_header(path):
    """Parse a header file and return list of binding descriptors."""
    bindings = []
    lines = Path(path).read_text(encoding='utf-8').splitlines()
    i = 0
    while i < len(lines):
        m = BIND_RE.search(lines[i])
        if m:
            lua_name = m.group(1)
            params_str = m.group(2)
            ret_type = m.group(3)
            # Next non-empty line should be the C++ declaration.
            j = i + 1
            cpp_func = None
            while j < len(lines):
                line = lines[j].strip()
                if line and not line.startswith('//'):
                    fm = FUNC_RE.search(line)
                    if fm:
                        cpp_func = fm.group(1)
                    break
                j += 1
            if cpp_func is None:
                raise ValueError(f"No function declaration found after LUA_BIND at line {i+1}")
            params = parse_params(params_str)
            bindings.append({
                'lua_name': lua_name,
                'cpp_func': cpp_func,
                'params': params,
                'ret_type': ret_type,
                'source': str(path),
                'line': i + 1,
            })
        i += 1
    return bindings


def generate_wrapper(binding):
    """Generate a Lua C API wrapper function for a binding."""
    lua_name = binding['lua_name']
    cpp_func = binding['cpp_func']
    params = binding['params']
    ret_type = binding['ret_type']

    lines = []
    lines.append(f'// Auto-generated from {binding["source"]}:{binding["line"]}')
    lines.append(f'int lua_generated_{lua_name}(lua_State* L) noexcept {{')

    # Generate argument extraction.
    arg_names = []
    uses_lua_state = False
    for idx, (name, typ) in enumerate(params):
        lua_idx = idx + 1  # Lua stack is 1-indexed.
        _, check_expr, _ = TYPE_MAP[typ]
        cpp_type = TYPE_MAP[typ][0]
        expr = check_expr.format(i=lua_idx)
        lines.append(f'  {cpp_type} {name} = {expr};')
        arg_names.append(name)
        uses_lua_state = True

    # Generate function call.
    args = ', '.join(arg_names)
    if ret_type == 'void':
        if not uses_lua_state:
            lines.append('  static_cast<void>(L);')
        lines.append(f'  {cpp_func}({args});')
        lines.append('  return 0;')
    else:
        if ret_type not in TYPE_MAP:
            raise ValueError(f"Unknown return type '{ret_type}'")
        ret_cpp = TYPE_MAP[ret_type][0]
        push_expr = TYPE_MAP[ret_type][2]
        lines.append(f'  {ret_cpp} result = {cpp_func}({args});')
        lines.append(f'  {push_expr};')
        lines.append('  return 1;')

    lines.append('}')
    return '\n'.join(lines)


def generate_registration(bindings):
    """Generate a function that registers all bindings into an 'engine' table."""
    lines = []
    lines.append('// Register all generated bindings into the table at stack top.')
    lines.append('void register_generated_bindings(lua_State* L) noexcept {')
    for b in bindings:
        lua_name = b['lua_name']
        lines.append(f'  lua_pushcfunction(L, &lua_generated_{lua_name});')
        lines.append(f'  lua_setfield(L, -2, "{lua_name}");')
    lines.append('}')
    return '\n'.join(lines)


def generate_output(bindings, input_headers):
    """Generate the full output .cpp file."""
    parts = []
    parts.append('// AUTO-GENERATED FILE — DO NOT EDIT.')
    parts.append('// Generated by tools/binding_generator/generate_bindings.py')
    parts.append(f'// From: {", ".join(str(h) for h in input_headers)}')
    parts.append('')
    parts.append('#include <cstdint>')
    parts.append('')
    parts.append('extern "C" {')
    parts.append('#include <lua.h>')
    parts.append('#include <lauxlib.h>')
    parts.append('}')
    parts.append('')
    # Forward-declare the C++ functions being wrapped.
    declared = set()
    for b in bindings:
        if b['cpp_func'] not in declared:
            # We include the header instead of forward-declaring.
            declared.add(b['cpp_func'])
    # Include all input headers.
    for h in input_headers:
        inc = h.replace('\\\\', '/').replace('\\', '/')
        parts.append(f'#include "{inc}"')
    parts.append('')
    parts.append('namespace engine::scripting {')
    parts.append('')
    parts.append('namespace {')
    parts.append('')
    for b in bindings:
        parts.append(generate_wrapper(b))
        parts.append('')
    parts.append('} // namespace')
    parts.append('')
    parts.append(generate_registration(bindings))
    parts.append('')
    parts.append('} // namespace engine::scripting')
    parts.append('')
    return '\n'.join(parts)


def main():
    parser = argparse.ArgumentParser(description='Lua binding generator')
    parser.add_argument('inputs', nargs='+', help='Input C++ header files')
    parser.add_argument('-o', '--output', required=True, help='Output .cpp file')
    args = parser.parse_args()

    all_bindings = []
    for inp in args.inputs:
        try:
            bindings = parse_header(inp)
            all_bindings.extend(bindings)
            print(f'  Parsed {len(bindings)} bindings from {inp}')
        except Exception as e:
            print(f'  ERROR parsing {inp}: {e}', file=sys.stderr)
            return 1

    if not all_bindings:
        print('  No LUA_BIND annotations found.')
        # Still write an empty registration function.
        output = (
            '// AUTO-GENERATED FILE — DO NOT EDIT.\n'
            '#include <cstdint>\n'
            'extern "C" {\n'
            '#include <lua.h>\n'
            '#include <lauxlib.h>\n'
            '}\n\n'
            'void register_generated_bindings(lua_State*) noexcept {}\n'
        )
    else:
        output = generate_output(all_bindings, args.inputs)

    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_text(output, encoding='utf-8')
    print(f'  Generated {len(all_bindings)} bindings -> {args.output}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
