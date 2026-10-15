#ifndef CSE_CONFIG_H_
#define CSE_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace cse {

struct Config {
  std::uint16_t port = 8080;
  std::size_t server_threads = 1;
  std::string dataset_path = "/data";
  std::string ollama_host = "ollama";
  std::string ollama_port = "11434";
  std::string ollama_model = "qwen2.5:0.5b";

  static Config FromEnv();
};

}  // namespace cse

#endif  // CSE_CONFIG_H_
