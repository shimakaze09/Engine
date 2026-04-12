// DAP (Debug Adapter Protocol) server for Lua debugging.
// Implements a subset of the DAP specification over TCP with JSON messages.
// https://microsoft.github.io/debug-adapter-protocol/

#include "engine/scripting/dap_server.h"
#include "engine/scripting/scripting.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstdio>
#include <cstring>

#include "engine/core/json.h"
#include "engine/core/logging.h"

// ---------- Platform socket abstraction ----------

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static constexpr SocketHandle kBadSocket = INVALID_SOCKET;
static bool g_wsaInitialized = false;

static bool platform_init_sockets() noexcept {
  if (g_wsaInitialized) {
    return true;
  }
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return false;
  }
  g_wsaInitialized = true;
  return true;
}

static void platform_shutdown_sockets() noexcept {
  if (g_wsaInitialized) {
    WSACleanup();
    g_wsaInitialized = false;
  }
}

static void platform_close_socket(SocketHandle s) noexcept {
  if (s != kBadSocket) {
    closesocket(s);
  }
}

static bool platform_set_nonblocking(SocketHandle s) noexcept {
  unsigned long mode = 1;
  return ioctlsocket(s, static_cast<long>(FIONBIO), &mode) == 0;
}

static bool platform_set_blocking(SocketHandle s) noexcept {
  unsigned long mode = 0;
  return ioctlsocket(s, static_cast<long>(FIONBIO), &mode) == 0;
}

#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle kBadSocket = -1;

static bool platform_init_sockets() noexcept { return true; }
static void platform_shutdown_sockets() noexcept {}

static void platform_close_socket(SocketHandle s) noexcept {
  if (s != kBadSocket) {
    close(s);
  }
}

static bool platform_set_nonblocking(SocketHandle s) noexcept {
  const int flags = fcntl(s, F_GETFL, 0);
  return fcntl(s, F_SETFL, flags | O_NONBLOCK) != -1;
}

static bool platform_set_blocking(SocketHandle s) noexcept {
  const int flags = fcntl(s, F_GETFL, 0);
  return fcntl(s, F_SETFL, flags & ~O_NONBLOCK) != -1;
}

#endif

namespace engine::scripting {

namespace {

// ---------- Server state ----------

static SocketHandle g_listenSocket = kBadSocket;
static SocketHandle g_clientSocket = kBadSocket;
static int g_seq = 1;

// Receive buffer for DAP messages.
static constexpr std::size_t kRecvBufferSize = 64U * 1024U;
static char g_recvBuffer[kRecvBufferSize]{};
static std::size_t g_recvUsed = 0U;

// ---------- Utility: JSON response helpers ----------

// Write a minimal DAP response header into a JsonWriter.
void write_response_header(core::JsonWriter &w, int requestSeq,
                           const char *command, bool success) noexcept {
  w.begin_object();
  w.write_uint("seq", static_cast<std::uint32_t>(g_seq++));
  w.write_string("type", "response");
  w.write_uint("request_seq", static_cast<std::uint32_t>(requestSeq));
  w.write_bool("success", success);
  w.write_string("command", command);
}

void write_event_header(core::JsonWriter &w, const char *event) noexcept {
  w.begin_object();
  w.write_uint("seq", static_cast<std::uint32_t>(g_seq++));
  w.write_string("type", "event");
  w.write_string("event", event);
}

// ---------- Transport ----------

bool send_dap_message(const char *json, std::size_t len) noexcept {
  if (g_clientSocket == kBadSocket) {
    return false;
  }
  char header[64]{};
  const int headerLen =
      std::snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
  if (headerLen <= 0) {
    return false;
  }
  // Send header.
  const auto hLen = static_cast<std::size_t>(headerLen);
  std::size_t sent = 0U;
  while (sent < hLen) {
    const auto n =
        send(g_clientSocket, header + sent, static_cast<int>(hLen - sent), 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  // Send body.
  sent = 0U;
  while (sent < len) {
    const auto n =
        send(g_clientSocket, json + sent, static_cast<int>(len - sent), 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool send_json_writer(core::JsonWriter &w) noexcept {
  if (w.failed()) {
    return false;
  }
  return send_dap_message(w.result(), w.result_size());
}

// Try to extract a complete DAP message from the receive buffer.
// Returns the number of bytes consumed (0 if no complete message).
// If successful, *outBody points into g_recvBuffer at the JSON start,
// and *outBodyLen is the JSON length (not null-terminated).
std::size_t try_extract_message(const char **outBody,
                                std::size_t *outBodyLen) noexcept {
  // Look for "Content-Length: <number>\r\n\r\n".
  const char *haystack = g_recvBuffer;
  const char *needle = "Content-Length: ";
  const std::size_t needleLen = 16U;
  const char *found = nullptr;
  if (g_recvUsed >= needleLen) {
    for (std::size_t i = 0U; i <= g_recvUsed - needleLen; ++i) {
      if (std::memcmp(haystack + i, needle, needleLen) == 0) {
        found = haystack + i;
        break;
      }
    }
  }
  if (found == nullptr) {
    return 0U;
  }

  // Parse content length.
  const char *numStart = found + needleLen;
  int contentLength = 0;
  {
    // Parse integer manually to avoid sscanf deprecation on MSVC.
    const char *p = numStart;
    while (*p >= '0' && *p <= '9') {
      contentLength = contentLength * 10 + (*p - '0');
      ++p;
    }
    if (p == numStart || contentLength <= 0) {
      return 0U;
    }
  }

  // Find the header/body separator "\r\n\r\n".
  const char *sep = nullptr;
  const std::size_t remaining =
      g_recvUsed - static_cast<std::size_t>(found - g_recvBuffer);
  for (std::size_t i = 0U; i + 3U < remaining; ++i) {
    if (found[i] == '\r' && found[i + 1] == '\n' && found[i + 2] == '\r' &&
        found[i + 3] == '\n') {
      sep = found + i;
      break;
    }
  }
  if (sep == nullptr) {
    return 0U;
  }

  const char *bodyStart = sep + 4;
  const std::size_t headerSize =
      static_cast<std::size_t>(bodyStart - g_recvBuffer);
  const auto bodyLen = static_cast<std::size_t>(contentLength);
  if (headerSize + bodyLen > g_recvUsed) {
    return 0U; // Not enough data yet.
  }

  *outBody = bodyStart;
  *outBodyLen = bodyLen;
  return headerSize + bodyLen;
}

// Receive data from the client into the recv buffer.
// Returns false if the connection was closed.
bool recv_into_buffer() noexcept {
  if (g_clientSocket == kBadSocket) {
    return false;
  }
  if (g_recvUsed >= kRecvBufferSize) {
    return false; // Buffer full.
  }
  const auto n = recv(g_clientSocket, g_recvBuffer + g_recvUsed,
                      static_cast<int>(kRecvBufferSize - g_recvUsed), 0);
  if (n <= 0) {
    return false;
  }
  g_recvUsed += static_cast<std::size_t>(n);
  return true;
}

// Consume bytes from the front of the recv buffer.
void consume_recv_buffer(std::size_t count) noexcept {
  if (count >= g_recvUsed) {
    g_recvUsed = 0U;
  } else {
    std::memmove(g_recvBuffer, g_recvBuffer + count, g_recvUsed - count);
    g_recvUsed -= count;
  }
}

// ---------- DAP command handlers ----------

void handle_initialize(int requestSeq) noexcept {
  core::JsonWriter w;
  write_response_header(w, requestSeq, "initialize", true);
  w.write_key("body");
  w.begin_object();
  w.write_bool("supportsConfigurationDoneRequest", true);
  w.write_bool("supportsEvaluateForHovers", true);
  w.end_object();
  w.end_object();
  send_json_writer(w);

  // Send initialized event.
  core::JsonWriter ev;
  write_event_header(ev, "initialized");
  ev.end_object();
  send_json_writer(ev);
}

void handle_launch(int requestSeq) noexcept {
  core::JsonWriter w;
  write_response_header(w, requestSeq, "launch", true);
  w.end_object();
  send_json_writer(w);
}

void handle_configuration_done(int requestSeq) noexcept {
  core::JsonWriter w;
  write_response_header(w, requestSeq, "configurationDone", true);
  w.end_object();
  send_json_writer(w);
}

void handle_set_breakpoints(int requestSeq, const core::JsonParser &parser,
                            const core::JsonValue &args) noexcept {
  // Replace current breakpoints with incoming list.
  debugger_clear_breakpoints();

  // Read source path.
  const core::JsonValue *srcObj = parser.get_object_field(args, "source");
  const char *sourcePath = nullptr;
  std::size_t sourcePathLen = 0U;
  if (srcObj != nullptr) {
    const core::JsonValue *pathVal = parser.get_object_field(*srcObj, "path");
    if (pathVal != nullptr) {
      parser.as_string(*pathVal, &sourcePath, &sourcePathLen);
    }
  }

  // Read breakpoints array.
  const core::JsonValue *bpArray = parser.get_object_field(args, "breakpoints");

  // Build a temporary source path (null-terminated).
  char srcPath[256]{};
  if (sourcePath != nullptr && sourcePathLen > 0U) {
    const std::size_t copyLen = sourcePathLen < sizeof(srcPath) - 1U
                                    ? sourcePathLen
                                    : sizeof(srcPath) - 1U;
    std::memcpy(srcPath, sourcePath, copyLen);
  }

  // Build response with verified breakpoints.
  core::JsonWriter w;
  write_response_header(w, requestSeq, "setBreakpoints", true);
  w.write_key("body");
  w.begin_object();
  w.begin_array("breakpoints");

  if (bpArray != nullptr) {
    const std::size_t count = parser.array_size(*bpArray);
    for (std::size_t i = 0U; i < count; ++i) {
      const core::JsonValue *bp = parser.get_array_element(*bpArray, i);
      if (bp == nullptr) {
        continue;
      }
      const core::JsonValue *lineVal = parser.get_object_field(*bp, "line");
      std::uint32_t line = 0U;
      if (lineVal != nullptr) {
        parser.as_uint(*lineVal, &line);
      }
      static_cast<void>(
          debugger_add_breakpoint(srcPath, static_cast<int>(line)));
      w.begin_object();
      w.write_bool("verified", true);
      w.write_uint("line", line);
      w.end_object();
    }
  }

  w.end_array();
  w.end_object();
  w.end_object();
  send_json_writer(w);
}

void handle_threads(int requestSeq) noexcept {
  core::JsonWriter w;
  write_response_header(w, requestSeq, "threads", true);
  w.write_key("body");
  w.begin_object();
  w.begin_array("threads");
  w.begin_object();
  w.write_uint("id", 1U);
  w.write_string("name", "main");
  w.end_object();
  w.end_array();
  w.end_object();
  w.end_object();
  send_json_writer(w);
}

void handle_stack_trace(int requestSeq, lua_State *L) noexcept {
  core::JsonWriter w;
  write_response_header(w, requestSeq, "stackTrace", true);
  w.write_key("body");
  w.begin_object();
  w.begin_array("stackFrames");

  if (L != nullptr) {
    lua_Debug ar{};
    int depth = 0;
    while (lua_getstack(L, depth, &ar) != 0) {
      lua_getinfo(L, "Sln", &ar);
      const char *name = (ar.name != nullptr) ? ar.name : "<unknown>";
      const char *source = (ar.source != nullptr) ? ar.source : "?";
      if (source[0] == '@') {
        ++source;
      }

      w.begin_object();
      w.write_uint("id", static_cast<std::uint32_t>(depth));
      w.write_string("name", name);
      w.write_key("source");
      w.begin_object();
      w.write_string("path", source);
      w.end_object();
      w.write_uint("line", static_cast<std::uint32_t>(
                               ar.currentline > 0 ? ar.currentline : 0));
      w.write_uint("column", 0U);
      w.end_object();
      ++depth;
    }
  }

  w.end_array();
  w.end_object();
  w.end_object();
  send_json_writer(w);
}

void handle_scopes(int requestSeq, int frameId) noexcept {
  // We expose 3 scopes: Locals, Upvalues, Globals.
  // The variablesReference encodes (frameId * 3 + scopeType).
  core::JsonWriter w;
  write_response_header(w, requestSeq, "scopes", true);
  w.write_key("body");
  w.begin_object();
  w.begin_array("scopes");

  // Locals
  w.begin_object();
  w.write_string("name", "Locals");
  w.write_uint("variablesReference",
               static_cast<std::uint32_t>(frameId * 3 + 1));
  w.write_bool("expensive", false);
  w.end_object();

  // Upvalues
  w.begin_object();
  w.write_string("name", "Upvalues");
  w.write_uint("variablesReference",
               static_cast<std::uint32_t>(frameId * 3 + 2));
  w.write_bool("expensive", false);
  w.end_object();

  // Globals
  w.begin_object();
  w.write_string("name", "Globals");
  w.write_uint("variablesReference",
               static_cast<std::uint32_t>(frameId * 3 + 3));
  w.write_bool("expensive", true);
  w.end_object();

  w.end_array();
  w.end_object();
  w.end_object();
  send_json_writer(w);
}

// Write a single Lua stack value as a DAP variable value string.
void format_lua_value(lua_State *L, int index, char *buf,
                      std::size_t bufSize) noexcept {
  const int t = lua_type(L, index);
  switch (t) {
  case LUA_TNUMBER:
    if (lua_isinteger(L, index)) {
      std::snprintf(buf, bufSize, "%lld",
                    static_cast<long long>(lua_tointeger(L, index)));
    } else {
      std::snprintf(buf, bufSize, "%g",
                    static_cast<double>(lua_tonumber(L, index)));
    }
    break;
  case LUA_TBOOLEAN:
    std::snprintf(buf, bufSize, "%s",
                  lua_toboolean(L, index) ? "true" : "false");
    break;
  case LUA_TSTRING: {
    const char *s = lua_tostring(L, index);
    std::snprintf(buf, bufSize, "\"%s\"", s != nullptr ? s : "");
  } break;
  case LUA_TNIL:
    std::snprintf(buf, bufSize, "nil");
    break;
  case LUA_TTABLE:
    std::snprintf(buf, bufSize, "table: %p",
                  static_cast<const void *>(lua_topointer(L, index)));
    break;
  case LUA_TFUNCTION:
    std::snprintf(buf, bufSize, "function: %p",
                  static_cast<const void *>(lua_topointer(L, index)));
    break;
  default:
    std::snprintf(buf, bufSize, "%s", lua_typename(L, t));
    break;
  }
}

void handle_variables(int requestSeq, int varRef, lua_State *L) noexcept {
  // Decode: frameId = (varRef - 1) / 3, scopeType = (varRef - 1) % 3
  const int frameId = (varRef - 1) / 3;
  const int scopeType = (varRef - 1) % 3; // 0=locals, 1=upvalues, 2=globals

  core::JsonWriter w;
  write_response_header(w, requestSeq, "variables", true);
  w.write_key("body");
  w.begin_object();
  w.begin_array("variables");

  if (L != nullptr) {
    lua_Debug ar{};
    if (scopeType == 0 && lua_getstack(L, frameId, &ar) != 0) {
      // Locals
      for (int n = 1;; ++n) {
        const char *name = lua_getlocal(L, &ar, n);
        if (name == nullptr) {
          break;
        }
        // Skip internal variables (start with '(').
        if (name[0] == '(') {
          lua_pop(L, 1);
          continue;
        }
        char valueBuf[256]{};
        format_lua_value(L, -1, valueBuf, sizeof(valueBuf));
        w.begin_object();
        w.write_string("name", name);
        w.write_string("value", valueBuf);
        w.write_string("type", luaL_typename(L, -1));
        w.write_uint("variablesReference", 0U);
        w.end_object();
        lua_pop(L, 1);
      }
    } else if (scopeType == 1 && lua_getstack(L, frameId, &ar) != 0) {
      // Upvalues
      lua_getinfo(L, "f", &ar);
      if (lua_isfunction(L, -1)) {
        for (int n = 1;; ++n) {
          const char *name = lua_getupvalue(L, -1, n);
          if (name == nullptr) {
            break;
          }
          char valueBuf[256]{};
          format_lua_value(L, -1, valueBuf, sizeof(valueBuf));
          w.begin_object();
          w.write_string("name", name);
          w.write_string("value", valueBuf);
          w.write_string("type", luaL_typename(L, -1));
          w.write_uint("variablesReference", 0U);
          w.end_object();
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1); // pop the function
    } else if (scopeType == 2) {
      // Globals — list a limited set of non-function globals.
      lua_pushglobaltable(L);
      lua_pushnil(L);
      int count = 0;
      while (lua_next(L, -2) != 0 && count < 50) {
        if (lua_type(L, -2) == LUA_TSTRING) {
          const char *name = lua_tostring(L, -2);
          // Skip internal/standard library names.
          if (name != nullptr && name[0] != '_') {
            char valueBuf[256]{};
            format_lua_value(L, -1, valueBuf, sizeof(valueBuf));
            w.begin_object();
            w.write_string("name", name);
            w.write_string("value", valueBuf);
            w.write_string("type", luaL_typename(L, -1));
            w.write_uint("variablesReference", 0U);
            w.end_object();
            ++count;
          }
        }
        lua_pop(L, 1); // pop value, keep key
      }
      lua_pop(L, 1); // pop globals table
    }
  }

  w.end_array();
  w.end_object();
  w.end_object();
  send_json_writer(w);
}

void handle_evaluate(int requestSeq, lua_State *L,
                     const core::JsonParser &parser,
                     const core::JsonValue &args) noexcept {
  const core::JsonValue *exprVal = parser.get_object_field(args, "expression");
  const char *exprStr = nullptr;
  std::size_t exprLen = 0U;
  if (exprVal != nullptr) {
    parser.as_string(*exprVal, &exprStr, &exprLen);
  }

  char expr[512]{};
  if (exprStr != nullptr && exprLen > 0U) {
    const std::size_t copyLen =
        exprLen < sizeof(expr) - 1U ? exprLen : sizeof(expr) - 1U;
    std::memcpy(expr, exprStr, copyLen);
  }

  char resultBuf[512] = "nil";
  const char *typeName = "nil";

  if (L != nullptr && expr[0] != '\0') {
    // Wrap in "return (...)" to evaluate as expression.
    char chunk[600]{};
    std::snprintf(chunk, sizeof(chunk), "return (%s)", expr);

    // Disable hooks during eval to avoid re-entry.
    lua_sethook(L, nullptr, 0, 0);

    const int loadStatus = luaL_loadstring(L, chunk);
    if (loadStatus == LUA_OK) {
      const int callStatus = lua_pcall(L, 0, 1, 0);
      if (callStatus == LUA_OK) {
        format_lua_value(L, -1, resultBuf, sizeof(resultBuf));
        typeName = luaL_typename(L, -1);
        lua_pop(L, 1);
      } else {
        const char *err = lua_tostring(L, -1);
        std::snprintf(resultBuf, sizeof(resultBuf), "<error: %s>",
                      err != nullptr ? err : "?");
        typeName = "error";
        lua_pop(L, 1);
      }
    } else {
      lua_pop(L, 1);
      std::snprintf(resultBuf, sizeof(resultBuf), "<parse error>");
      typeName = "error";
    }
  }

  core::JsonWriter w;
  write_response_header(w, requestSeq, "evaluate", true);
  w.write_key("body");
  w.begin_object();
  w.write_string("result", resultBuf);
  w.write_string("type", typeName);
  w.write_uint("variablesReference", 0U);
  w.end_object();
  w.end_object();
  send_json_writer(w);
}

void handle_disconnect(int requestSeq) noexcept {
  core::JsonWriter w;
  write_response_header(w, requestSeq, "disconnect", true);
  w.end_object();
  send_json_writer(w);

  platform_close_socket(g_clientSocket);
  g_clientSocket = kBadSocket;
  g_recvUsed = 0U;
}

// Process a single DAP message. Returns the step mode if a continue/step
// command was received, or Continue with *outResume=false for other commands.
DapStepMode process_message(const char *body, std::size_t bodyLen, lua_State *L,
                            bool *outResume) noexcept {
  *outResume = false;

  core::JsonParser parser;
  if (!parser.parse(body, bodyLen)) {
    return DapStepMode::Continue;
  }
  const core::JsonValue *root = parser.root();
  if (root == nullptr) {
    return DapStepMode::Continue;
  }

  // Read "command" and "seq".
  const core::JsonValue *cmdVal = parser.get_object_field(*root, "command");
  const char *cmdStr = nullptr;
  std::size_t cmdLen = 0U;
  if (cmdVal != nullptr) {
    parser.as_string(*cmdVal, &cmdStr, &cmdLen);
  }

  const core::JsonValue *seqVal = parser.get_object_field(*root, "seq");
  std::uint32_t seq = 0U;
  if (seqVal != nullptr) {
    parser.as_uint(*seqVal, &seq);
  }
  const int requestSeq = static_cast<int>(seq);

  // Read "arguments".
  core::JsonValue argsStorage{};
  const core::JsonValue *argsVal = parser.get_object_field(*root, "arguments");
  if (argsVal != nullptr) {
    argsStorage = *argsVal;
  }

  if (cmdStr == nullptr || cmdLen == 0U) {
    return DapStepMode::Continue;
  }

  // Compare command strings.
  auto cmd_eq = [&](const char *expected) -> bool {
    return std::strlen(expected) == cmdLen &&
           std::memcmp(cmdStr, expected, cmdLen) == 0;
  };

  if (cmd_eq("initialize")) {
    handle_initialize(requestSeq);
  } else if (cmd_eq("launch") || cmd_eq("attach")) {
    handle_launch(requestSeq);
  } else if (cmd_eq("configurationDone")) {
    handle_configuration_done(requestSeq);
  } else if (cmd_eq("setBreakpoints")) {
    handle_set_breakpoints(requestSeq, parser, argsStorage);
  } else if (cmd_eq("threads")) {
    handle_threads(requestSeq);
  } else if (cmd_eq("stackTrace")) {
    handle_stack_trace(requestSeq, L);
  } else if (cmd_eq("scopes")) {
    std::uint32_t frameId = 0U;
    if (argsVal != nullptr) {
      const core::JsonValue *fid = parser.get_object_field(*argsVal, "frameId");
      if (fid != nullptr) {
        parser.as_uint(*fid, &frameId);
      }
    }
    handle_scopes(requestSeq, static_cast<int>(frameId));
  } else if (cmd_eq("variables")) {
    std::uint32_t varRef = 0U;
    if (argsVal != nullptr) {
      const core::JsonValue *vr =
          parser.get_object_field(*argsVal, "variablesReference");
      if (vr != nullptr) {
        parser.as_uint(*vr, &varRef);
      }
    }
    handle_variables(requestSeq, static_cast<int>(varRef), L);
  } else if (cmd_eq("evaluate")) {
    handle_evaluate(requestSeq, L, parser, argsStorage);
  } else if (cmd_eq("continue")) {
    core::JsonWriter w;
    write_response_header(w, requestSeq, "continue", true);
    w.write_key("body");
    w.begin_object();
    w.write_bool("allThreadsContinued", true);
    w.end_object();
    w.end_object();
    send_json_writer(w);
    *outResume = true;
    return DapStepMode::Continue;
  } else if (cmd_eq("next")) {
    core::JsonWriter w;
    write_response_header(w, requestSeq, "next", true);
    w.end_object();
    send_json_writer(w);
    *outResume = true;
    return DapStepMode::Next;
  } else if (cmd_eq("stepIn")) {
    core::JsonWriter w;
    write_response_header(w, requestSeq, "stepIn", true);
    w.end_object();
    send_json_writer(w);
    *outResume = true;
    return DapStepMode::StepIn;
  } else if (cmd_eq("stepOut")) {
    core::JsonWriter w;
    write_response_header(w, requestSeq, "stepOut", true);
    w.end_object();
    send_json_writer(w);
    *outResume = true;
    return DapStepMode::StepOut;
  } else if (cmd_eq("disconnect")) {
    handle_disconnect(requestSeq);
    *outResume = true; // Resume execution, client gone.
    return DapStepMode::Continue;
  } else {
    // Unknown command — send error response.
    core::JsonWriter w;
    write_response_header(w, requestSeq, cmdStr != nullptr ? cmdStr : "unknown",
                          false);
    w.write_string("message", "unsupported command");
    w.end_object();
    send_json_writer(w);
  }

  return DapStepMode::Continue;
}

} // namespace

// ---------- Public API ----------

bool dap_start(std::uint16_t port) noexcept {
  if (g_listenSocket != kBadSocket) {
    return true; // Already running.
  }
  if (!platform_init_sockets()) {
    core::log_message(core::LogLevel::Error, "dap",
                      "failed to initialize sockets");
    return false;
  }

  g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (g_listenSocket == kBadSocket) {
    core::log_message(core::LogLevel::Error, "dap",
                      "failed to create listen socket");
    return false;
  }

  // Allow address reuse.
  int optVal = 1;
  setsockopt(g_listenSocket, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char *>(&optVal),
             static_cast<int>(sizeof(optVal)));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);

  if (bind(g_listenSocket, reinterpret_cast<struct sockaddr *>(&addr),
           sizeof(addr)) != 0) {
    core::log_message(core::LogLevel::Error, "dap",
                      "failed to bind listen socket");
    platform_close_socket(g_listenSocket);
    g_listenSocket = kBadSocket;
    return false;
  }

  if (listen(g_listenSocket, 1) != 0) {
    core::log_message(core::LogLevel::Error, "dap",
                      "failed to listen on socket");
    platform_close_socket(g_listenSocket);
    g_listenSocket = kBadSocket;
    return false;
  }

  if (!platform_set_nonblocking(g_listenSocket)) {
    core::log_message(core::LogLevel::Error, "dap",
                      "failed to set non-blocking");
    platform_close_socket(g_listenSocket);
    g_listenSocket = kBadSocket;
    return false;
  }

  char msg[64]{};
  std::snprintf(msg, sizeof(msg), "DAP server listening on port %u",
                static_cast<unsigned>(port));
  core::log_message(core::LogLevel::Info, "dap", msg);
  return true;
}

void dap_stop() noexcept {
  if (g_clientSocket != kBadSocket) {
    platform_close_socket(g_clientSocket);
    g_clientSocket = kBadSocket;
  }
  if (g_listenSocket != kBadSocket) {
    platform_close_socket(g_listenSocket);
    g_listenSocket = kBadSocket;
  }
  g_recvUsed = 0U;
  g_seq = 1;
  platform_shutdown_sockets();
}

bool dap_is_running() noexcept { return g_listenSocket != kBadSocket; }

bool dap_has_client() noexcept { return g_clientSocket != kBadSocket; }

void dap_poll() noexcept {
  if (g_listenSocket == kBadSocket) {
    return;
  }
  // Accept new client if none connected.
  if (g_clientSocket == kBadSocket) {
    g_clientSocket = accept(g_listenSocket, nullptr, nullptr);
    if (g_clientSocket != kBadSocket) {
      core::log_message(core::LogLevel::Info, "dap", "DAP client connected");
      g_recvUsed = 0U;
      // Client socket stays blocking for the message processing loop.
    }
  }
  // Try to process any already-buffered or incoming messages (non-blocking).
  if (g_clientSocket != kBadSocket) {
    platform_set_nonblocking(g_clientSocket);
    recv_into_buffer();
    const char *body = nullptr;
    std::size_t bodyLen = 0U;
    const std::size_t consumed = try_extract_message(&body, &bodyLen);
    if (consumed > 0U) {
      bool resume = false;
      process_message(body, bodyLen, nullptr, &resume);
      consume_recv_buffer(consumed);
    }
  }
}

DapStepMode dap_on_stopped(lua_State *L, const char * /*source*/, int /*line*/,
                           const char *reason) noexcept {
  if (g_clientSocket == kBadSocket) {
    return DapStepMode::Continue;
  }

  // Send "stopped" event.
  core::JsonWriter ev;
  write_event_header(ev, "stopped");
  ev.write_key("body");
  ev.begin_object();
  ev.write_string("reason", reason != nullptr ? reason : "breakpoint");
  ev.write_uint("threadId", 1U);
  ev.write_bool("allThreadsStopped", true);
  ev.end_object();
  ev.end_object();
  send_json_writer(ev);

  // Enter blocking message loop — process DAP requests until continue/step.
  platform_set_blocking(g_clientSocket);

  DapStepMode mode = DapStepMode::Continue;
  for (;;) {
    // Try to extract from buffer first.
    const char *body = nullptr;
    std::size_t bodyLen = 0U;
    std::size_t consumed = try_extract_message(&body, &bodyLen);
    if (consumed > 0U) {
      bool resume = false;
      mode = process_message(body, bodyLen, L, &resume);
      consume_recv_buffer(consumed);
      if (resume) {
        break;
      }
      continue;
    }

    // Need more data.
    if (!recv_into_buffer()) {
      // Connection lost — resume execution.
      platform_close_socket(g_clientSocket);
      g_clientSocket = kBadSocket;
      g_recvUsed = 0U;
      break;
    }
  }

  return mode;
}

} // namespace engine::scripting
