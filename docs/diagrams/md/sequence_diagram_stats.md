@startuml
actor Admin
participant "RequestHandler" as Handler
participant "IndexSnapshot\n(current)" as Snap

Admin -> Handler : GET /api/stats
activate Handler
Handler -> Handler : snap = activeSnapshot.load()
alt index not built yet (snap == nullptr)
    Handler --> Admin : 200 OK {"documents": 0, "index_build_time_ms": 0, "threads_used": 0}
else
    Handler -> Snap : stats
    activate Snap
    Snap --> Handler : IndexStats {documents, buildTimeMs, threadsUsed}
    deactivate Snap
    Handler --> Admin : 200 OK {"documents": D, "index_build_time_ms": T, "threads_used": N}
end
deactivate Handler
@enduml
