@startuml
skinparam componentStyle rectangle

node "User machine" as UserHost {
  artifact "Web browser" as Browser
  artifact "Python CLI\n(./search)" as CLI
}

node "Docker host (docker compose)" as DockerHost {
  node "web\n<<container: nginx:alpine>>" as Web {
    artifact "Web UI static files\n(HTML/CSS/JS)" as UI
    component "Reverse proxy\n/api, /ws -> server:8080" as Proxy
  }

  node "server\n<<container: C++20 search server>>" as Server {
    component "SearchServer\n(Boost.Beast HTTP + WS)" as SS
    component "Network ThreadPool" as NetPool
    component "Indexer ThreadPool (N)" as IdxPool
    component "ConcurrentInvertedIndex\n(in RAM)" as Index
    component "OllamaService\n(1-thread pool)" as OSvc
  }

  node "ollama\n<<container: ollama/ollama>>" as Ollama {
    component "Ollama API :11434\n(qwen2.5:0.5b)" as OAPI
  }

  node "ollama-init\n<<one-shot container>>" as OInit {
    artifact "ollama pull qwen2.5:0.5b" as Pull
  }

  database "./data (bind mount, ro)\nProject Gutenberg .txt" as Data
  database "ollama_models\n(named volume)" as Models
}

Browser --> Web : HTTP :3000 / WebSocket
CLI --> Server : HTTP / WebSocket :8080
Proxy --> SS : HTTP / WS :8080
SS --> OSvc
OSvc --> OAPI : HTTP POST /api/generate\nGET /api/tags
OInit --> OAPI : pull model
OAPI --> Models
IdxPool --> Data : read files (/data)
SS --> Index
IdxPool --> Index
SS --> NetPool
@enduml
