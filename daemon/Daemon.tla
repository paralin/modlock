---- MODULE Daemon ----
\* Serve/bindStarting/bind, stopIdle and lockFileContext; client.go's
\* connect/start path. Daemon locks last until socket close or process death.
\* Startup/idle serialization through start.lock is outside this model. A late lock
\* acquired after cancellation is closed by lockFileContext's cleanup.
\* The model has two independent OS processes sharing one state directory;
\* it does not assume a POSIX process lock excludes threads in one process.
\* Callers: modlock daemon command and each command's daemon client. Socket
\* byte transport, updates and arbitrary unlinking of lock files are outside it.
EXTENDS Naturals, FiniteSets
CONSTANT KeepLock
Processes == {1, 2}
VARIABLE s
Init == s = [held |-> {}, listening |-> {}, cancelled |-> {}, late |-> {}, stopped |-> {}]
Acquire(i) == /\ i \notin s.held /\ s.held = {} /\ i \notin s.stopped
              /\ s' = [s EXCEPT !.held = @ \cup {i}]
Listen(i) == /\ i \in s.held /\ i \notin s.cancelled /\ i \notin s.listening
             /\ s' = [s EXCEPT !.listening = @ \cup {i},
                       !.held = IF KeepLock THEN @ ELSE @ \ {i}]
Cancel(i) == /\ i \notin s.cancelled /\ i \notin s.listening
             /\ s' = [s EXCEPT !.cancelled = @ \cup {i}]
Late(i) == /\ i \in s.held /\ i \in s.cancelled
           /\ s' = [s EXCEPT !.held = @ \ {i}, !.late = @ \cup {i}, !.stopped = @ \cup {i}]
Stop(i) == /\ i \in s.listening
           /\ s' = [s EXCEPT !.listening = @ \ {i}, !.held = @ \ {i}, !.stopped = @ \cup {i}]
Next == (\E i \in Processes : Acquire(i) \/ Listen(i) \/ Cancel(i) \/ Late(i) \/ Stop(i)) \/ UNCHANGED s
Spec == Init /\ [][Next]_s
FairSpec == Spec /\ \A i \in Processes : WF_s(Late(i))
OneDaemon == Cardinality(s.listening) <= 1
ListenerHoldsLock == s.listening \subseteq s.held
CancelledLockReleased == \A i \in Processes : (i \in s.held /\ i \in s.cancelled) ~> i \notin s.held
====
