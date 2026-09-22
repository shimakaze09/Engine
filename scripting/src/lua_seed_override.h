/*
** Pins Lua's string-hash seed. Force-included into every engine_lua
** translation unit (the compilers' -include / /FI), so lstate.c finds the
** macro already defined and never compiles its own seed, which mixes
** time(NULL) with two heap addresses.
**
** That default makes table iteration order over string keys differ per
** process. Order is not a language guarantee, but it is observable, and a
** script that walks a table to build simulation state would produce a
** different world on every run. Pinning it costs the hash-collision
** defence the randomization provides, which protects a server taking
** untrusted keys from a wedged hash table -- not this engine's threat
** model, where scripts are author-local and trusted
** (docs/decisions/0003, 0019).
**
** C, not C++: this is compiled as part of vendored Lua.
*/

#ifndef ENGINE_LUA_SEED_OVERRIDE_H
#define ENGINE_LUA_SEED_OVERRIDE_H

#ifndef luai_makeseed
/* The state pointer is the seed's only input in the default and the
** reason it varies; consumed and discarded so the signature still
** matches. The constant is the 32-bit golden ratio, chosen only for
** being a recognizable non-zero. */
#define luai_makeseed(L) ((void)(L), 0x9E3779B9u)
#endif

#endif /* ENGINE_LUA_SEED_OVERRIDE_H */
