#include "Config/Config.h"
#include "Ollama/OllamaService.h"
#include "Server/RequestHandler.h"
#include "Server/SearchServer.h"
#include <exception>
#include <iostream>

int main()
{
    try
    {
        const Config config = Config::fromEnv();

        OllamaService ollamaService(config.ollamaHost, config.ollamaPort, config.ollamaModel, config.ollamaTimeoutMs);
        RequestHandler requestHandler(ollamaService);
        SearchServer server(config.port, config.serverThreads, requestHandler);

        std::cout << "Ollama configured at " << config.ollamaHost << ":" << config.ollamaPort
                  << " (model=" << config.ollamaModel << ")" << std::endl;

        server.start();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
