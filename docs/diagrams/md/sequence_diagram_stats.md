@startuml
actor Admin
participant "SearchServer" as Server
participant "IndexSnapshot\n(current)" as Snap

Admin -> Server : GET /api/stats
activate Server
Server -> Server : snap = snapshot.load()
alt index not built yet (snap == nullptr)
    Server --> Admin : 200 OK {"documents": 0, "index_build_time_ms": 0, "threads_used": 0}
else
    Server -> Snap : stats
    activate Snap
    Snap --> Server : IndexStats {documents, build_time_ms, threads_used}
    deactivate Snap
    Server --> Admin : 200 OK {"documents": D, "index_build_time_ms": T, "threads_used": N}
end
deactivate Server
@enduml
