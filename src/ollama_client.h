#ifndef CSE_OLLAMA_CLIENT_H_
#define CSE_OLLAMA_CLIENT_H_

#include <chrono>
#include <string>

namespace cse {

class OllamaClient {
 public:
  OllamaClient(std::string host, std::string port, std::string model);

  bool CheckHealth(std::chrono::milliseconds timeout) const;

  const std::string& model() const { return model_; }

 private:
  std::string host_;
  std::string port_;
  std::string model_;
};

}  // namespace cse

#endif  // CSE_OLLAMA_CLIENT_H_
