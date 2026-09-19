// Integration test for Lua DAP debugger (P1-M2-G1h).
// Test: mock DAP client sets breakpoint, script pauses, stackTrace line
// matches, out-of-range and zero frame/variable references are refused
// with failure responses while in-range ones list the paused frame's
// scopes and locals (#457), a nonterminating evaluate ends in a bounded
// error response while the session stays usable for a further evaluate
// and continue.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <thread>

#include "engine/core/service_locator.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/dap_server.h"
#include "engine/scripting/scripting.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

static const char *kTempScript = "dap_breakpoint_test.lua";
static constexpr std::uint16_t kDapPort = 47125;

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

/// Writes script data.
bool write_script(const char *code) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, kTempScript, "w") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(kTempScript, "w");
  if (f == nullptr) {
    return false;
  }
#endif
  std::fputs(code, f);
  std::fclose(f);
  return true;
}

void remove_script() noexcept { std::remove(kTempScript); }

void close_socket_safe(SocketHandle s) noexcept {
  if (s == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  closesocket(s);
#else
  close(s);
#endif
}

/// Starts client-side socket support for the DAP integration test.
bool init_client_socket_platform() noexcept {
#if defined(_WIN32)
  WSADATA wsa{};
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
  return true;
#endif
}

/// Stops client-side socket support for the DAP integration test.
void shutdown_client_socket_platform() noexcept {
#if defined(_WIN32)
  WSACleanup();
#endif
}

/// Connects a client socket to the local DAP server with a short retry
/// window; a non-zero receiveBufferBytes shrinks the client's receive
/// window first so a large server response cannot be absorbed unread.
bool connect_to_dap_server(SocketHandle *outSock,
                           int receiveBufferBytes = 0) noexcept {
  if (outSock == nullptr) {
    return false;
  }
  *outSock = kInvalidSocket;

  SocketHandle sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == kInvalidSocket) {
    return false;
  }
  if (receiveBufferBytes > 0) {
    static_cast<void>(setsockopt(
        sock, SOL_SOCKET, SO_RCVBUF,
        reinterpret_cast<const char *>(&receiveBufferBytes),
        sizeof(receiveBufferBytes)));
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kDapPort);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  for (int i = 0; i < 120; ++i) {
    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
      *outSock = sock;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  close_socket_safe(sock);
  return false;
}

bool send_all(SocketHandle s, const char *data, std::size_t len) noexcept {
  std::size_t sent = 0U;
  while (sent < len) {
#if defined(_WIN32)
    const int n = send(s, data + sent, static_cast<int>(len - sent), 0);
#else
    const int n = static_cast<int>(send(s, data + sent, len - sent, 0));
#endif
    if (n <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool send_dap_request(SocketHandle s, int seq, const char *command,
                      const char *argumentsJson) noexcept {
  char body[2048] = {};
  if (argumentsJson == nullptr) {
    std::snprintf(body, sizeof(body),
                  "{\"seq\":%d,\"type\":\"request\",\"command\":\"%s\"}", seq,
                  command);
  } else {
    std::snprintf(
        body, sizeof(body),
        "{\"seq\":%d,\"type\":\"request\",\"command\":\"%s\",\"arguments\":%s}",
        seq, command, argumentsJson);
  }

  char header[128] = {};
  const int headerLen =
      std::snprintf(header, sizeof(header), "Content-Length: %u\r\n\r\n",
                    static_cast<unsigned>(std::strlen(body)));
  if (headerLen <= 0) {
    return false;
  }

  return send_all(s, header, static_cast<std::size_t>(headerLen)) &&
         send_all(s, body, std::strlen(body));
}

bool try_extract_dap_message(std::string *buffer,
                             std::string *outBody) noexcept {
  const std::size_t headerEnd = buffer->find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    return false;
  }

  const std::string header = buffer->substr(0, headerEnd);
  const std::string key = "Content-Length:";
  const std::size_t kpos = header.find(key);
  if (kpos == std::string::npos) {
    return false;
  }

  std::size_t p = kpos + key.size();
  while (p < header.size() && header[p] == ' ') {
    ++p;
  }

  std::size_t len = 0U;
  while (p < header.size() && header[p] >= '0' && header[p] <= '9') {
    len = len * 10U + static_cast<std::size_t>(header[p] - '0');
    ++p;
  }

  const std::size_t bodyStart = headerEnd + 4U;
  if (buffer->size() < bodyStart + len) {
    return false;
  }

  *outBody = buffer->substr(bodyStart, len);
  buffer->erase(0, bodyStart + len);
  return true;
}

bool recv_dap_message(SocketHandle s, std::string *buffer, std::string *outBody,
                      int timeoutMs) noexcept {
  // The socket wait is bounded so a debugger that never answers fails the
  // test instead of hanging it; the elapsed time is never asserted.
  // wall-clock: harness-timeout
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

  // wall-clock: harness-timeout
  while (std::chrono::steady_clock::now() < deadline) {
    if (try_extract_dap_message(buffer, outBody)) {
      return true;
    }

#if defined(_WIN32)
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 50 * 1000;
    const int sel = select(0, &rfds, nullptr, nullptr, &tv);
#else
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 50 * 1000;
    const int sel = select(s + 1, &rfds, nullptr, nullptr, &tv);
#endif
    if (sel <= 0) {
      continue;
    }

    char chunk[1024] = {};
#if defined(_WIN32)
    const int n = recv(s, chunk, static_cast<int>(sizeof(chunk)), 0);
#else
    const int n = static_cast<int>(recv(s, chunk, sizeof(chunk), 0));
#endif
    if (n <= 0) {
      return false;
    }
    buffer->append(chunk, static_cast<std::size_t>(n));
  }

  return false;
}

struct ClientResult {
  bool connected = false;
  bool stoppedEventSeen = false;
  bool stackLineMatched = false;
  bool referenceBoundariesRejected = false;
  bool validReferencesAccepted = false;
  bool firstEvaluateAnswered = false;
  bool evaluateBoundedErrorSeen = false;
  bool evaluateAfterwardsOk = false;
  bool continueAckSeen = false;
};

/// Sends one request while the thread is paused and waits for its own
/// response, skipping any event delivered in between; false when the
/// server disconnects or the bounded wait expires.
bool exchange_request(SocketHandle sock, std::string *recvBuffer, int seq,
                      const char *command, const char *argumentsJson,
                      std::string *outBody) noexcept {
  if (!send_dap_request(sock, seq, command, argumentsJson)) {
    return false;
  }
  char seqToken[64] = {};
  std::snprintf(seqToken, sizeof(seqToken), "\"request_seq\":%d,\"success\":",
                seq);
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!recv_dap_message(sock, recvBuffer, outBody, 5000)) {
      return false;
    }
    if (outBody->find(seqToken) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool response_failed(const std::string &body) noexcept {
  return body.find("\"success\":false") != std::string::npos;
}

bool response_succeeded(const std::string &body) noexcept {
  return body.find("\"success\":true") != std::string::npos;
}

/// Regression for #457: identifiers that would overflow the signed
/// reference arithmetic, name no frame, or are DAP's zero sentinel come
/// back as failure responses, and the in-range references for the paused
/// frame still resolve to its three scopes and its locals. Every request
/// is answered, so the session is intact for the evaluate that follows.
bool probe_reference_boundaries(SocketHandle sock, std::string *recvBuffer,
                                int *seq, ClientResult *result) noexcept {
  std::string body;
  bool rejected = true;
  // Signed-boundary frame id: frameId * 3 + 1 overflows int.
  rejected = exchange_request(sock, recvBuffer, (*seq)++, "scopes",
                              "{\"frameId\":2147483647}", &body) &&
             response_failed(body) && rejected;
  // Maximum 32-bit frame id.
  rejected = exchange_request(sock, recvBuffer, (*seq)++, "scopes",
                              "{\"frameId\":4294967295}", &body) &&
             response_failed(body) && rejected;
  // In range for the arithmetic, but deeper than the paused stack.
  rejected = exchange_request(sock, recvBuffer, (*seq)++, "scopes",
                              "{\"frameId\":4096}", &body) &&
             response_failed(body) && rejected;
  // Zero is DAP's "no children" sentinel, never a scope.
  rejected = exchange_request(sock, recvBuffer, (*seq)++, "variables",
                              "{\"variablesReference\":0}", &body) &&
             response_failed(body) && rejected;
  // Maximum reference: decodes to a frame far beyond the stack.
  rejected = exchange_request(sock, recvBuffer, (*seq)++, "variables",
                              "{\"variablesReference\":4294967295}", &body) &&
             response_failed(body) && rejected;
  result->referenceBoundariesRejected = rejected;

  bool accepted = true;
  accepted = exchange_request(sock, recvBuffer, (*seq)++, "scopes",
                              "{\"frameId\":0}", &body) &&
             response_succeeded(body) &&
             (body.find("\"variablesReference\":1,") != std::string::npos) &&
             (body.find("\"variablesReference\":2,") != std::string::npos) &&
             (body.find("\"variablesReference\":3,") != std::string::npos) &&
             accepted;
  // Reference 1 is frame 0's Locals scope; the paused function declares
  // the local `value`.
  accepted = exchange_request(sock, recvBuffer, (*seq)++, "variables",
                              "{\"variablesReference\":1}", &body) &&
             response_succeeded(body) &&
             (body.find("\"name\":\"value\"") != std::string::npos) &&
             accepted;
  result->validReferencesAccepted = accepted;
  return true;
}

/// Runs the configured command, loop, or tool for mock dap client.
void run_mock_dap_client(int breakpointLine, ClientResult *result) noexcept {
  if (result == nullptr) {
    return;
  }

  if (!init_client_socket_platform()) {
    return;
  }

  SocketHandle sock = kInvalidSocket;
  if (!connect_to_dap_server(&sock)) {
    shutdown_client_socket_platform();
    return;
  }

  result->connected = true;

  int seq = 1;
  if (!send_dap_request(sock, seq++, "initialize", "{}")) {
    close_socket_safe(sock);
#if defined(_WIN32)
    WSACleanup();
#endif
    return;
  }
  if (!send_dap_request(sock, seq++, "launch", "{}")) {
    close_socket_safe(sock);
#if defined(_WIN32)
    WSACleanup();
#endif
    return;
  }
  if (!send_dap_request(sock, seq++, "configurationDone", "{}")) {
    close_socket_safe(sock);
#if defined(_WIN32)
    WSACleanup();
#endif
    return;
  }

  char bpArgs[512] = {};
  std::snprintf(
      bpArgs, sizeof(bpArgs),
      "{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":%d}]}",
      kTempScript, breakpointLine);
  if (!send_dap_request(sock, seq++, "setBreakpoints", bpArgs)) {
    close_socket_safe(sock);
#if defined(_WIN32)
    WSACleanup();
#endif
    return;
  }

  // A setBreakpoints request for a different source must not clear the one
  // above: replacement is per-source in DAP.
  const char *otherArgs = "{\"source\":{\"path\":\"dap_other_source.lua\"},"
                          "\"breakpoints\":[{\"line\":1}]}";
  if (!send_dap_request(sock, seq++, "setBreakpoints", otherArgs)) {
    close_socket_safe(sock);
#if defined(_WIN32)
    WSACleanup();
#endif
    return;
  }

  std::string recvBuffer;
  bool waiting = true;
  while (waiting) {
    std::string body;
    if (!recv_dap_message(sock, &recvBuffer, &body, 5000)) {
      break;
    }

    if (body.find("\"event\":\"stopped\"") != std::string::npos) {
      result->stoppedEventSeen = true;
      if (!send_dap_request(sock, seq++, "stackTrace", "{\"threadId\":1}")) {
        break;
      }
      continue;
    }

    if (body.find("\"command\":\"stackTrace\"") != std::string::npos) {
      char lineToken[32] = {};
      std::snprintf(lineToken, sizeof(lineToken), "\"line\":%d",
                    breakpointLine);
      if (body.find(lineToken) != std::string::npos) {
        result->stackLineMatched = true;
      }
      if (!probe_reference_boundaries(sock, &recvBuffer, &seq, result)) {
        break;
      }
      // Regression for #454: an expression that never returns must come
      // back as a bounded error response rather than holding the paused
      // main thread (before the fix this request never got a reply).
      if (!send_dap_request(
              sock, seq++, "evaluate",
              "{\"expression\":\"(function() while true do end end)()\"}")) {
        break;
      }
      continue;
    }

    if (body.find("\"command\":\"evaluate\"") != std::string::npos) {
      if (!result->firstEvaluateAnswered) {
        result->firstEvaluateAnswered = true;
        result->evaluateBoundedErrorSeen =
            (body.find("instruction budget") != std::string::npos) &&
            (body.find("\"type\":\"error\"") != std::string::npos);
        // The session survives the bounded failure: the next evaluate
        // computes normally on the same paused thread.
        if (!send_dap_request(sock, seq++, "evaluate",
                              "{\"expression\":\"20 + 22\"}")) {
          break;
        }
        continue;
      }
      result->evaluateAfterwardsOk =
          body.find("\"result\":\"42\"") != std::string::npos;
      if (!send_dap_request(sock, seq++, "continue", "{\"threadId\":1}")) {
        break;
      }
      continue;
    }

    if (body.find("\"command\":\"continue\"") != std::string::npos) {
      result->continueAckSeen = true;
      waiting = false;
      break;
    }
  }

  close_socket_safe(sock);
  shutdown_client_socket_platform();
}

/// Verifies DAP stop clears client state and releases the listen socket.
bool test_dap_restart_clears_session() noexcept {
  if (!engine::scripting::dap_start(kDapPort)) {
    return false;
  }

  if (!init_client_socket_platform()) {
    engine::scripting::dap_stop();
    return false;
  }

  SocketHandle sock = kInvalidSocket;
  if (!connect_to_dap_server(&sock)) {
    shutdown_client_socket_platform();
    engine::scripting::dap_stop();
    return false;
  }

  bool accepted = false;
  for (int i = 0; i < 50; ++i) {
    engine::scripting::dap_poll();
    if (engine::scripting::dap_has_client()) {
      accepted = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const char *partialFrame = "Content-Length: 64\r\n\r\n{\"seq\":1";
  const bool partialSent =
      accepted && send_all(sock, partialFrame, std::strlen(partialFrame));
  if (partialSent) {
    engine::scripting::dap_poll();
  }

  engine::scripting::dap_stop();
  close_socket_safe(sock);
  shutdown_client_socket_platform();

  const bool stopped = !engine::scripting::dap_is_running() &&
                       !engine::scripting::dap_has_client();
  const bool restarted = stopped && engine::scripting::dap_start(kDapPort) &&
                         engine::scripting::dap_is_running() &&
                         !engine::scripting::dap_has_client();
  engine::scripting::dap_stop();

  return accepted && partialSent && restarted &&
         !engine::scripting::dap_is_running() &&
         !engine::scripting::dap_has_client();
}

bool wait_for_dap_client(bool expectedConnected) noexcept {
  for (int i = 0; i < 50; ++i) {
    engine::scripting::dap_poll();
    if (engine::scripting::dap_has_client() == expectedConnected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

bool send_malformed_dap_frame_and_expect_disconnect(const char *frame) noexcept {
  if (frame == nullptr) {
    return false;
  }

  if (!engine::scripting::dap_start(kDapPort)) {
    return false;
  }

  if (!init_client_socket_platform()) {
    engine::scripting::dap_stop();
    return false;
  }

  SocketHandle sock = kInvalidSocket;
  if (!connect_to_dap_server(&sock)) {
    shutdown_client_socket_platform();
    engine::scripting::dap_stop();
    return false;
  }

  const bool accepted = wait_for_dap_client(true);
  const bool sent = accepted && send_all(sock, frame, std::strlen(frame));
  const bool disconnected = sent && wait_for_dap_client(false);

  close_socket_safe(sock);
  shutdown_client_socket_platform();
  engine::scripting::dap_stop();
  return accepted && sent && disconnected;
}

bool test_dap_rejects_oversized_content_length() noexcept {
  return send_malformed_dap_frame_and_expect_disconnect(
      "Content-Length: 70000\r\n\r\n{}");
}

bool test_dap_rejects_overflowing_content_length() noexcept {
  return send_malformed_dap_frame_and_expect_disconnect(
      "Content-Length: 999999999999999999999999999999\r\n\r\n{}");
}

/// Verifies an unknown command is echoed as its exact bounded JSON string.
bool test_dap_unknown_command_echo() noexcept {
  if (!engine::scripting::dap_start(kDapPort) ||
      !init_client_socket_platform()) {
    engine::scripting::dap_stop();
    return false;
  }

  SocketHandle sock = kInvalidSocket;
  if (!connect_to_dap_server(&sock)) {
    shutdown_client_socket_platform();
    engine::scripting::dap_stop();
    return false;
  }

  const bool accepted = wait_for_dap_client(true);
  const bool sent =
      accepted && send_dap_request(sock, 77, "mysteryCommand", "{}");

  std::string receiveBuffer{};
  std::string response{};
  bool received = false;
  for (int attempt = 0; sent && (attempt < 50); ++attempt) {
    engine::scripting::dap_poll();
    if (recv_dap_message(sock, &receiveBuffer, &response, 20)) {
      received = true;
      break;
    }
  }

  close_socket_safe(sock);
  shutdown_client_socket_platform();
  engine::scripting::dap_stop();

  constexpr const char *kExpected =
      "{\"seq\":1,\"type\":\"response\",\"request_seq\":77,"
      "\"success\":false,\"command\":\"mysteryCommand\","
      "\"message\":\"unsupported command\"}";
  return accepted && sent && received && (response == kExpected);
}

bool test_dap_breakpoint_pause() noexcept {
  if (!engine::scripting::initialize_scripting()) {
    return false;
  }

  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    engine::scripting::shutdown_scripting();
    return false;
  }
  engine::core::ServiceLocator serviceLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);

  const char *script = "engine.debugger_enable(true)\n"
                       "function dap_target()\n"
                       "  local value = 1\n"
                       "  value = value + 1\n"
                       "  if value == 2 then\n"
                       "    value = value + 1\n"
                       "  end\n"
                       "end\n";

  if (!write_script(script)) {
    engine::scripting::shutdown_scripting();
    return false;
  }

  const int breakpointLine = 4; // value = value + 1
  // The evaluation budget is the sandbox instruction limit; a small one
  // keeps the nonterminating expression's refusal quick.
  engine::scripting::set_instruction_limit(50000);

  if (!engine::scripting::dap_start(kDapPort)) {
    remove_script();
    engine::scripting::shutdown_scripting();
    return false;
  }

  ClientResult clientResult{};
  std::thread clientThread(run_mock_dap_client, breakpointLine, &clientResult);

  if (!engine::scripting::load_script(kTempScript)) {
    clientThread.join();
    engine::scripting::dap_stop();
    remove_script();
    engine::scripting::shutdown_scripting();
    return false;
  }

  // The transport is serviced by the pipeline's scripting stage in
  // production; this harness polls it directly between frames.
  for (int i = 0; i < 100; ++i) {
    engine::scripting::set_frame_time(0.016F, 0.016F * static_cast<float>(i));
    engine::scripting::dap_poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const bool callOk = engine::scripting::call_script_function("dap_target");

  clientThread.join();
  engine::scripting::dap_stop();
  remove_script();
  engine::scripting::shutdown_scripting();

  const bool ok = callOk && clientResult.connected &&
                  clientResult.stoppedEventSeen &&
                  clientResult.stackLineMatched &&
                  clientResult.referenceBoundariesRejected &&
                  clientResult.validReferencesAccepted &&
                  clientResult.evaluateBoundedErrorSeen &&
                  clientResult.evaluateAfterwardsOk &&
                  clientResult.continueAckSeen;
  if (!ok) {
    std::printf("\n    callOk=%d connected=%d stopped=%d line=%d "
                "refsRejected=%d refsAccepted=%d "
                "evalBounded=%d evalAfter=%d continue=%d\n",
                callOk ? 1 : 0, clientResult.connected ? 1 : 0,
                clientResult.stoppedEventSeen ? 1 : 0,
                clientResult.stackLineMatched ? 1 : 0,
                clientResult.referenceBoundariesRejected ? 1 : 0,
                clientResult.validReferencesAccepted ? 1 : 0,
                clientResult.evaluateBoundedErrorSeen ? 1 : 0,
                clientResult.evaluateAfterwardsOk ? 1 : 0,
                clientResult.continueAckSeen ? 1 : 0);
  }
  return ok;
}

} // namespace

/// Regression for #539: a setBreakpoints list longer than the parser's
/// pointer scratch used to be cut short silently (the missing entries
/// were neither set nor reported), so every entry must come back, and an
/// entry the breakpoint store cannot hold answers verified:false instead
/// of claiming success.
bool test_dap_large_breakpoint_list() noexcept {
  constexpr std::size_t kBreakpoints = 700U;
  if (!engine::scripting::dap_start(kDapPort)) {
    return false;
  }
  if (!init_client_socket_platform()) {
    engine::scripting::dap_stop();
    return false;
  }
  SocketHandle sock = kInvalidSocket;
  if (!connect_to_dap_server(&sock) || !wait_for_dap_client(true)) {
    close_socket_safe(sock);
    shutdown_client_socket_platform();
    engine::scripting::dap_stop();
    return false;
  }

  std::string body = "{\"seq\":1,\"type\":\"request\",\"command\":"
                     "\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":"
                     "\"dap_many_breakpoints.lua\"},\"breakpoints\":[";
  char entry[32] = {};
  for (std::size_t i = 0U; i < kBreakpoints; ++i) {
    std::snprintf(entry, sizeof(entry), "%s{\"line\":%zu}", (i == 0U) ? "" : ",",
                  i + 1U);
    body += entry;
  }
  body += "]}}";
  char header[64] = {};
  const int headerLen =
      std::snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n",
                    body.size());
  bool sent = (headerLen > 0) &&
              send_all(sock, header, static_cast<std::size_t>(headerLen)) &&
              send_all(sock, body.data(), body.size());

  // The server reads and answers on dap_poll, so poll between short
  // bounded waits until the response arrives.
  std::string recvBuffer;
  std::string response;
  bool answered = false;
  for (int attempt = 0; sent && !answered && (attempt < 200); ++attempt) {
    engine::scripting::dap_poll();
    std::string message;
    if (recv_dap_message(sock, &recvBuffer, &message, 20) &&
        (message.find("\"command\":\"setBreakpoints\"") != std::string::npos)) {
      response = message;
      answered = true;
    }
  }

  // Breakpoints outlive the session, so hand the store back before the
  // next check: an empty list for the same source replaces them.
  const char *clearArgs = "{\"source\":{\"path\":\"dap_many_breakpoints.lua\"},"
                          "\"breakpoints\":[]}";
  bool cleared = false;
  if (answered && send_dap_request(sock, 2, "setBreakpoints", clearArgs)) {
    for (int attempt = 0; !cleared && (attempt < 200); ++attempt) {
      engine::scripting::dap_poll();
      std::string message;
      if (recv_dap_message(sock, &recvBuffer, &message, 20) &&
          (message.find("\"request_seq\":2,") != std::string::npos)) {
        cleared = true;
      }
    }
  }

  engine::scripting::dap_stop();
  close_socket_safe(sock);
  shutdown_client_socket_platform();
  if (!answered || !cleared) {
    std::printf(answered ? "(breakpoints not cleared) "
                         : "(no setBreakpoints response) ");
    return false;
  }

  std::size_t entries = 0U;
  std::size_t verified = 0U;
  std::size_t unverified = 0U;
  for (std::size_t pos = response.find("\"line\":"); pos != std::string::npos;
       pos = response.find("\"line\":", pos + 1U)) {
    ++entries;
  }
  for (std::size_t pos = response.find("\"verified\":true");
       pos != std::string::npos;
       pos = response.find("\"verified\":true", pos + 1U)) {
    ++verified;
  }
  for (std::size_t pos = response.find("\"verified\":false");
       pos != std::string::npos;
       pos = response.find("\"verified\":false", pos + 1U)) {
    ++unverified;
  }
  // The store holds far fewer than 700, so some are refused; every entry
  // is answered and none is claimed beyond what the store took.
  const bool ok = (entries == kBreakpoints) &&
                  ((verified + unverified) == kBreakpoints) &&
                  (verified >= 1U) && (unverified >= 1U);
  if (!ok) {
    std::printf("(entries=%zu verified=%zu unverified=%zu) ", entries,
                verified, unverified);
  }
  return ok;
}

bool stopped_bridge_is_playing() noexcept { return false; }
bool stopped_bridge_is_paused() noexcept { return false; }

/// Walks upward from the current path until the bundled assets are found.
bool set_working_directory_with_assets() noexcept {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    std::error_code ec{};
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      continue;
    }
    if (std::filesystem::exists(normalized / "assets/main.lua", ec) &&
        std::filesystem::exists(normalized / "assets/shaders/bgfx/shaders.json",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

/// Regression for #540: the transport was serviced only from
/// set_frame_time, which the pipeline calls while playing, so a client
/// could connect before Play but never get its initialize answered. The
/// production pipeline (headless, editor bridge reporting Stopped) must
/// answer initialize and setBreakpoints in the same handshake without a
/// Play, and drain both in one frame.
bool test_dap_attach_while_stopped() noexcept {
  if (!set_working_directory_with_assets()) {
    std::printf("(assets not found) ");
    return false;
  }
  engine::runtime::EditorBridge bridge{};
  bridge.is_playing = &stopped_bridge_is_playing;
  bridge.is_paused = &stopped_bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    engine::runtime::set_editor_bridge(nullptr);
    std::printf("(bootstrap failed) ");
    return false;
  }
  bool ok = false;
  {
    engine::EnginePipeline pipeline;
    if (pipeline.initialize(0U) && engine::scripting::dap_start(kDapPort) &&
        init_client_socket_platform()) {
      SocketHandle sock = kInvalidSocket;
      if (connect_to_dap_server(&sock)) {
        // Both requests are on the wire before any frame runs.
        const bool sent =
            send_dap_request(sock, 1, "initialize", "{\"adapterID\":\"t\"}") &&
            send_dap_request(sock, 2, "setBreakpoints",
                             "{\"source\":{\"path\":\"dap_stopped.lua\"},"
                             "\"breakpoints\":[{\"line\":1}]}");
        std::string recvBuffer;
        bool initAnswered = false;
        bool breakpointsAnswered = false;
        for (int frame = 0; sent && (frame < 30) &&
                            !(initAnswered && breakpointsAnswered);
             ++frame) {
          if (!pipeline.execute_frame()) {
            break;
          }
          std::string body;
          while (recv_dap_message(sock, &recvBuffer, &body, 20)) {
            if (body.find("\"request_seq\":1,") != std::string::npos) {
              initAnswered = true;
            }
            if (body.find("\"request_seq\":2,") != std::string::npos) {
              breakpointsAnswered = true;
            }
          }
        }
        ok = initAnswered && breakpointsAnswered;
        if (!ok) {
          std::printf("(initialize %d, setBreakpoints %d) ",
                      initAnswered ? 1 : 0, breakpointsAnswered ? 1 : 0);
        }
        close_socket_safe(sock);
      }
      shutdown_client_socket_platform();
    }
    engine::scripting::dap_stop();
    pipeline.teardown();
  }
  engine::shutdown();
  engine::runtime::set_editor_bridge(nullptr);
  return ok;
}

/// Regression for #576 row 1: the client socket is non-blocking, so a
/// response larger than what the kernel accepts at once used to stop at
/// the first EAGAIN with a half-written frame on the wire and the
/// handler none the wiser. The client here keeps a tiny receive window and
/// only starts reading after the server has begun sending; it must then
/// receive the whole frame, or find the session closed, never a torn
/// frame.
bool test_dap_large_response_never_truncated() noexcept {
  constexpr std::size_t kBreakpoints = 2000U;
  if (!engine::scripting::dap_start(kDapPort)) {
    return false;
  }
  if (!init_client_socket_platform()) {
    engine::scripting::dap_stop();
    return false;
  }
  SocketHandle sock = kInvalidSocket;
  if (!connect_to_dap_server(&sock, 2048) || !wait_for_dap_client(true)) {
    close_socket_safe(sock);
    shutdown_client_socket_platform();
    engine::scripting::dap_stop();
    return false;
  }

  std::string body = "{\"seq\":1,\"type\":\"request\",\"command\":"
                     "\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":"
                     "\"dap_big_response.lua\"},\"breakpoints\":[";
  char entry[32] = {};
  for (std::size_t i = 0U; i < kBreakpoints; ++i) {
    std::snprintf(entry, sizeof(entry), "%s{\"line\":%zu}", (i == 0U) ? "" : ",",
                  i + 1U);
    body += entry;
  }
  body += "]}}";
  char header[64] = {};
  const int headerLen =
      std::snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n",
                    body.size());
  const bool sent = (headerLen > 0) &&
                    send_all(sock, header, static_cast<std::size_t>(headerLen)) &&
                    send_all(sock, body.data(), body.size());

  // The reader starts late and drains slowly, and the first three writes
  // are forced to report would-block; the server's send must either wait
  // and finish the frame or close the session.
  engine::scripting::dap_inject_would_block(3);
  enum class Outcome { Complete, Closed, Torn, TimedOut };
  Outcome outcome = Outcome::TimedOut;
  std::string received;
  std::thread reader([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::string buffer;
    std::string frame;
    if (recv_dap_message(sock, &buffer, &frame, 6000)) {
      received = frame;
      outcome = Outcome::Complete;
      return;
    }
    // recv_dap_message returns false on a closed socket or on timeout: a
    // readable socket that yields nothing is the peer's close.
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    timeval tv{};
    tv.tv_usec = 100 * 1000;
#if defined(_WIN32)
    const bool readable = select(0, &rfds, nullptr, nullptr, &tv) > 0;
#else
    const bool readable = select(sock + 1, &rfds, nullptr, nullptr, &tv) > 0;
#endif
    char probe = 0;
    const bool closed = readable && (recv(sock, &probe, 1, 0) <= 0);
    if (!buffer.empty()) {
      outcome = Outcome::Torn;
    } else {
      outcome = closed ? Outcome::Closed : Outcome::TimedOut;
    }
  });
  for (int attempt = 0; sent && (attempt < 400); ++attempt) {
    engine::scripting::dap_poll();
    if (!engine::scripting::dap_has_client()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  reader.join();

  bool ok = false;
  if (outcome == Outcome::Complete) {
    std::size_t entries = 0U;
    for (std::size_t pos = received.find("\"line\":"); pos != std::string::npos;
         pos = received.find("\"line\":", pos + 1U)) {
      ++entries;
    }
    ok = entries == kBreakpoints;
    if (!ok) {
      std::printf("(complete frame with %zu entries) ", entries);
    }
  } else if (outcome == Outcome::Closed) {
    ok = true; // a dropped session is the other acceptable outcome
  } else {
    std::printf(outcome == Outcome::Torn ? "(torn frame on the wire) "
                                         : "(no frame and no close) ");
  }

  // Release the store for later checks, on a fresh session if this one
  // was dropped.
  engine::scripting::dap_stop();
  close_socket_safe(sock);
  shutdown_client_socket_platform();
  return ok;
}

/// Runs this executable or test program.
int main() {
  std::printf("  dap_test::restart_clears_session ... ");
  const bool restartOk = test_dap_restart_clears_session();
  std::printf(restartOk ? "PASS\n" : "FAIL\n");

  std::printf("  dap_test::rejects_oversized_content_length ... ");
  const bool oversizedOk = test_dap_rejects_oversized_content_length();
  std::printf(oversizedOk ? "PASS\n" : "FAIL\n");

  std::printf("  dap_test::rejects_overflowing_content_length ... ");
  const bool overflowOk = test_dap_rejects_overflowing_content_length();
  std::printf(overflowOk ? "PASS\n" : "FAIL\n");

  std::printf("  dap_test::unknown_command_echo ... ");
  const bool unknownOk = test_dap_unknown_command_echo();
  std::printf(unknownOk ? "PASS\n" : "FAIL\n");

  std::printf("  dap_test::large_breakpoint_list ... ");
  const bool largeListOk = test_dap_large_breakpoint_list();
  std::printf(largeListOk ? "PASS\n" : "FAIL\n");

  std::printf("  dap_test::breakpoint_pause ... ");
  const bool breakpointOk = test_dap_breakpoint_pause();
  std::printf(breakpointOk ? "PASS\n" : "FAIL\n");

  std::printf("  dap_test::attach_while_stopped ... ");
  const bool attachOk = test_dap_attach_while_stopped();
  std::printf(attachOk ? "PASS\n" : "FAIL\n");

  std::printf("  dap_test::large_response_never_truncated ... ");
  const bool untornOk = test_dap_large_response_never_truncated();
  std::printf(untornOk ? "PASS\n" : "FAIL\n");
  return (restartOk && oversizedOk && overflowOk && unknownOk && largeListOk &&
          breakpointOk && attachOk && untornOk)
             ? 0
             : 1;
}
