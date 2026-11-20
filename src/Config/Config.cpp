#include "Config.h"
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace
{

std::string getEnv(const char* name, const std::string& fallback)
{
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? std::string(value) : fallback;
}

// splits "http://host:port[/path]" into host and port
void parseUrl(std::string_view url, std::string& host, std::string& port)
{
    constexpr std::string_view SCHEME_PREFIX = "http://";
    if (url.substr(0, SCHEME_PREFIX.size()) == SCHEME_PREFIX)
    {
        url.remove_prefix(SCHEME_PREFIX.size());
    }
    if (const auto slash = url.find('/'); slash != std::string_view::npos)
    {
        url = url.substr(0, slash);
    }
    if (const auto colon = url.rfind(':'); colon != std::string_view::npos)
    {
        host = std::string(url.substr(0, colon));
        port = std::string(url.substr(colon + 1));
    }
    else
    {
        host = std::string(url);
        port = "11434";
    }
    if (host.empty() || port.empty())
    {
        throw std::invalid_argument("Invalid OLLAMA_URL");
    }
}

} // namespace

Config Config::fromEnv()
{
    Config config;

    const int portValue = std::stoi(getEnv("PORT", "8080"));
    if (portValue <= 0 || portValue > 65535)
    {
        throw std::invalid_argument("PORT must be in [1, 65535]");
    }
    config.port = static_cast<uint16_t>(portValue);

    const unsigned int hw = std::thread::hardware_concurrency();
    const int threads = std::stoi(getEnv("SERVER_THREADS", std::to_string(hw == 0 ? 4 : hw)));
    if (threads <= 0)
    {
        throw std::invalid_argument("SERVER_THREADS must be positive");
    }
    config.serverThreads = threads;

    config.datasetPath = getEnv("DATASET_PATH", config.datasetPath);
    config.ollamaModel = getEnv("OLLAMA_MODEL", config.ollamaModel);
    parseUrl(getEnv("OLLAMA_URL", "http://ollama:11434"), config.ollamaHost, config.ollamaPort);

    const int timeoutMs = std::stoi(getEnv("OLLAMA_TIMEOUT_MS", "30000"));
    if (timeoutMs <= 0)
    {
        throw std::invalid_argument("OLLAMA_TIMEOUT_MS must be positive");
    }
    config.ollamaTimeoutMs = timeoutMs;

    const long long buckets = std::stoll(getEnv("INDEX_BUCKETS", "64"));
    if (buckets <= 0 || (buckets & (buckets - 1)) != 0)
    {
        throw std::invalid_argument("INDEX_BUCKETS must be a positive power of two");
    }
    config.indexBuckets = static_cast<size_t>(buckets);

    return config;
}
