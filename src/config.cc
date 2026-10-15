#include "config.h"

#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace cse {
namespace {

std::string GetEnv(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return (value != nullptr && *value != '\0') ? std::string(value) : fallback;
}

// Splits "http://host:port[/path]" into host and port.
void ParseUrl(std::string_view url, std::string& host, std::string& port) {
  constexpr std::string_view kScheme = "http://";
  if (url.substr(0, kScheme.size()) == kScheme) {
    url.remove_prefix(kScheme.size());
  }
  if (const auto slash = url.find('/'); slash != std::string_view::npos) {
    url = url.substr(0, slash);
  }
  if (const auto colon = url.rfind(':'); colon != std::string_view::npos) {
    host = std::string(url.substr(0, colon));
    port = std::string(url.substr(colon + 1));
  } else {
    host = std::string(url);
    port = "11434";
  }
  if (host.empty() || port.empty()) {
    throw std::invalid_argument("Invalid OLLAMA_URL");
  }
}

}  // namespace

Config Config::FromEnv() {
  Config config;

  const int port = std::stoi(GetEnv("PORT", "8080"));
  if (port <= 0 || port > 65535) {
    throw std::invalid_argument("PORT must be in [1, 65535]");
  }
  config.port = static_cast<std::uint16_t>(port);

  const unsigned hw = std::thread::hardware_concurrency();
  const int threads =
      std::stoi(GetEnv("SERVER_THREADS", std::to_string(hw == 0 ? 4 : hw)));
  if (threads <= 0) {
    throw std::invalid_argument("SERVER_THREADS must be positive");
  }
  config.server_threads = static_cast<std::size_t>(threads);

  config.dataset_path = GetEnv("DATASET_PATH", config.dataset_path);
  config.ollama_model = GetEnv("OLLAMA_MODEL", config.ollama_model);
  ParseUrl(GetEnv("OLLAMA_URL", "http://ollama:11434"), config.ollama_host,
           config.ollama_port);
  return config;
}

}  // namespace cse
