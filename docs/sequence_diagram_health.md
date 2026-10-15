@startuml
actor Admin
participant "SearchServer" as Server
participant "OllamaService" as OllamaSvc
participant "Ollama API\n(External)" as OllamaAPI

Admin -> Server : GET /api/health
activate Server
Server -> OllamaSvc : checkHealth()
activate OllamaSvc
OllamaSvc -> OllamaAPI : GET /api/tags (timeout 2 s)
activate OllamaAPI
alt Ollama reachable
    OllamaAPI --> OllamaSvc : 200 OK {"models": [...]}
    OllamaSvc -> OllamaSvc : configured model present?
    OllamaSvc --> Server : true / false
else connection refused / timeout
    OllamaAPI --> OllamaSvc : error
    OllamaSvc --> Server : false
end
deactivate OllamaAPI
deactivate OllamaSvc
Server --> Admin : 200 OK {"status": "ok", "ollama": true|false}
note right: Always 200 while the\nserver process is alive
deactivate Server
@enduml
