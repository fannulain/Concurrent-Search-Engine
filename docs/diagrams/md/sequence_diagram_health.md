@startuml
actor Admin
participant "RequestHandler" as Handler
participant "OllamaService" as OllamaSvc
participant "Ollama API\n(External)" as OllamaAPI

Admin -> Handler : GET /api/health
activate Handler
Handler -> OllamaSvc : checkHealth(2000ms)
activate OllamaSvc
OllamaSvc -> OllamaAPI : GET /api/tags (timeout 2 s)
activate OllamaAPI
alt Ollama reachable
    OllamaAPI --> OllamaSvc : 200 OK {"models": [...]}
    OllamaSvc -> OllamaSvc : configured model present?
    OllamaSvc --> Handler : true / false
else connection refused / timeout
    OllamaAPI --> OllamaSvc : error
    OllamaSvc --> Handler : false
end
deactivate OllamaAPI
deactivate OllamaSvc
Handler --> Admin : 200 OK {"status": "ok", "ollama": true|false}
note right: Always 200 while the\nserver process is alive
deactivate Handler
@enduml
