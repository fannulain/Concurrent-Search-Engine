#ifndef OLLAMASERVICE_H
#define OLLAMASERVICE_H

#include <chrono>
#include <string>

class OllamaService
{
private:
    std::string host;
    std::string port;
    std::string model;
    int timeoutMs;

public:
    OllamaService(std::string host, std::string port, std::string model, int timeoutMs = 30000);

    bool checkHealth(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const;

    const std::string& getModel() const { return model; }
};

#endif // OLLAMASERVICE_H
