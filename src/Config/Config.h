#ifndef CONFIG_H
#define CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>

struct Config
{
    uint16_t port = 8080;
    int serverThreads = 1;
    std::string datasetPath = "/data";
    std::string ollamaHost = "ollama";
    std::string ollamaPort = "11434";
    std::string ollamaModel = "qwen2.5:0.5b";
    int ollamaTimeoutMs = 30000;
    size_t indexBuckets = 64;

    static Config fromEnv();
};

#endif // CONFIG_H
