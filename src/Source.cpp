#include "Config/Config.h"
#include "Index/IndexerService.h"
#include "Index/Tokenizer.h"
#include "Ollama/OllamaService.h"
#include "Search/SearchEngine.h"
#include "Server/RequestHandler.h"
#include "Server/SearchServer.h"
#include <exception>
#include <iostream>

int main()
{
    try
    {
        const Config config = Config::fromEnv();

        Tokenizer tokenizer;
        IndexerService indexerService(tokenizer, config.datasetPath, config.indexBuckets);
        SearchEngine searchEngine(tokenizer);
        OllamaService ollamaService(config.ollamaHost, config.ollamaPort, config.ollamaModel, tokenizer,
                                    config.ollamaTimeoutMs);
        RequestHandler requestHandler(ollamaService, indexerService, searchEngine);
        SearchServer server(config.port, config.serverThreads, requestHandler);

        std::cout << "Dataset path: " << config.datasetPath << " (buckets=" << config.indexBuckets << ")" << std::endl;
        std::cout << "Ollama configured at " << config.ollamaHost << ":" << config.ollamaPort
                  << " (model=" << config.ollamaModel << ")" << std::endl;

        server.start();
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
