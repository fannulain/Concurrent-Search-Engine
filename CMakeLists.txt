cmake_minimum_required(VERSION 3.20)

project(ConcurrentSearchEngine
  VERSION 0.1.0
  DESCRIPTION "Concurrent search engine with LLM query expansion"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CSE_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(CSE_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)
find_package(Boost 1.81 REQUIRED CONFIG)
find_package(nlohmann_json 3.11 REQUIRED)

add_library(cse_core STATIC
  src/config.cc
  src/http_server.cc
  src/ollama_client.cc
  src/request_handler.cc
  src/thread_pool.cc
)
target_include_directories(cse_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cse_core PUBLIC
  Boost::headers
  nlohmann_json::nlohmann_json
  Threads::Threads
)
target_compile_options(cse_core PUBLIC
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

add_executable(search_server src/main.cc)
target_link_libraries(search_server PRIVATE cse_core)

install(TARGETS search_server RUNTIME DESTINATION bin)
