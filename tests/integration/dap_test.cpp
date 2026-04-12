// Integration test for Lua DAP debugger (P1-M2-G1h).
// Test: mock DAP client sets breakpoint, script pauses, stackTrace line
// matches.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>

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
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

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
  bool continueAckSeen = false;
};

void run_mock_dap_client(int breakpointLine, ClientResult *result) noexcept {
  if (result == nullptr) {
    return;
  }

#if defined(_WIN32)
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return;
  }
#endif

  SocketHandle sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == kInvalidSocket) {
#if defined(_WIN32)
    WSACleanup();
#endif
    return;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kDapPort);
#if defined(_WIN32)
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
#else
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
#endif

  bool connected = false;
  for (int i = 0; i < 120; ++i) {
    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!connected) {
    close_socket_safe(sock);
#if defined(_WIN32)
    WSACleanup();
#endif
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
#if defined(_WIN32)
  WSACleanup();
#endif
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
  engine::runtime::bind_scripting_runtime(world.get());

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

  // Give server time to accept/process initialize + breakpoint commands.
  for (int i = 0; i < 100; ++i) {
    engine::scripting::set_frame_time(0.016F, 0.016F * static_cast<float>(i));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const bool callOk = engine::scripting::call_script_function("dap_target");

  clientThread.join();
  engine::scripting::dap_stop();
  remove_script();
  engine::scripting::shutdown_scripting();

  const bool ok = callOk && clientResult.connected &&
                  clientResult.stoppedEventSeen &&
                  clientResult.stackLineMatched && clientResult.continueAckSeen;
  if (!ok) {
    std::printf("\n    callOk=%d connected=%d stopped=%d line=%d continue=%d\n",
                callOk ? 1 : 0, clientResult.connected ? 1 : 0,
                clientResult.stoppedEventSeen ? 1 : 0,
                clientResult.stackLineMatched ? 1 : 0,
                clientResult.continueAckSeen ? 1 : 0);
  }
  return ok;
}

} // namespace

int main() {
  std::printf("  dap_test::breakpoint_pause ... ");
  const bool ok = test_dap_breakpoint_pause();
  std::printf(ok ? "PASS\n" : "FAIL\n");
  return ok ? 0 : 1;
}
