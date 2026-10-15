#include <boost/asio.hpp>
#include <csignal>
#include <exception>
#include <iostream>
#include <memory>

#include "config.h"
#include "http_server.h"
#include "ollama_client.h"
#include "request_handler.h"
#include "thread_pool.h"

namespace net = boost::asio;
using tcp = net::ip::tcp;

int main() {
  try {
    const cse::Config config = cse::Config::FromEnv();

    cse::OllamaClient ollama(config.ollama_host, config.ollama_port,
                             config.ollama_model);
    cse::RequestHandler handler(ollama);

    net::io_context ioc(static_cast<int>(config.server_threads));
    std::make_shared<cse::Listener>(
        ioc, tcp::endpoint{net::ip::address_v4::any(), config.port}, handler)
        ->Run();

    net::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&ioc](const boost::system::error_code&, int signal) {
      std::cout << "Received signal " << signal << ", shutting down"
                << std::endl;
      ioc.stop();
    });

    std::cout << "Search server listening on 0.0.0.0:" << config.port
              << " with " << config.server_threads << " network threads"
              << " (ollama=" << config.ollama_host << ':' << config.ollama_port
              << ", model=" << config.ollama_model << ')' << std::endl;

    {
      cse::ThreadPool network_pool(config.server_threads);
      for (std::size_t i = 0; i < config.server_threads; ++i) {
        network_pool.Enqueue([&ioc] { ioc.run(); });
      }
    }

    std::cout << "Server stopped" << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 1;
  }
}
