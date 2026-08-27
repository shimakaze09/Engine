// Declares the packer's child-process launch: it runs a tool with an
// explicit argument vector and no shell in between, so authored strings the
// packer forwards to a tool (manifest defines, asset paths) reach that tool
// as data rather than as text a command interpreter parses.

#pragma once

#include <string>
#include <vector>

/// Outcome of one child-process run: whether the child started at all, and
/// the status it exited with when it did.
struct ProcessResult final {
  bool launched = false;
  int exitCode = -1;
};

/// Runs `executable` with `arguments` as its argument vector after argv[0]
/// and waits for it to exit. Every argument reaches the child verbatim: no
/// command interpreter, no word splitting, no expansion. With
/// `discardStandardError` set the child's standard error goes to the null
/// device; its standard output is always inherited.
ProcessResult run_process(const std::string &executable,
                          const std::vector<std::string> &arguments,
                          bool discardStandardError);
