@startuml
left to right direction
skinparam packageStyle rectangle

actor "Client (Searcher)" as Client
actor "Administrator (Researcher)" as Admin
actor "Ollama LLM" as Ollama << External System >>

rectangle "Search Engine System" {
  usecase "Keyword Search (REST, mode=keyword)" as UC_KeywordSearch
  usecase "Streaming Search (WebSocket)" as UC_WSSearch
  usecase "Build Dataset Index (POST /api/index)" as UC_Index
  usecase "Health Check (GET /api/health)" as UC_Health
  usecase "Get Statistics (GET /api/stats)" as UC_Stats
}

Client --> UC_KeywordSearch
Client --> UC_WSSearch

Admin --> UC_Index
Admin --> UC_Health
Admin --> UC_Stats

UC_WSSearch ..> Ollama : << includes query expansion >>
UC_Health ..> Ollama : << checks availability >>
@enduml