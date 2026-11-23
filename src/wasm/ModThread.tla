---- MODULE ModThread ----
\* A mod on its own thread beside the game thread: the frame window, the hop,
\* the borrow and the stop.
\*
\* The mod thread owns the mod's store and delivers events from a queue. Its
\* host calls take one of three paths: a write is buffered and answered at once,
\* a read is answered from the snapshot or else hops, and a hop waits for the
\* game thread. The game thread alone touches the engine. Each tick, after the
\* engine's frame, it opens a window: it applies the buffer, refreshes the
\* snapshot by replay, posts a Frame event and serves hops until the mod is
\* idle with an empty queue or the deadline passes. A decision (Damage and the
\* like) arrives mid-simulation and is answered in the game thread's own step:
\* the game thread borrows the store when the mod is idle, or gives the default
\* answer. A stop sets a flag, wakes the hop waiter and joins the thread.
\*
\* The game thread's phases are Engine (simulating), Applying (the window's
\* buffer being applied), Window (serving hops), Borrowed (a decision running
\* on the store in place), Joining and Joined. The mod thread's states are Idle,
\* Running (inside an event), WaitingHop and Done. Not modeled: what a call
\* returns, which methods are writes, reads or hops (a call is one of those
\* three), time except as the deadline, and more than one mod.
\*
\* Requirements this spec places on the implementation:
\*  - Claiming the store (the mod popping an event, the game borrowing it) is
\*    one atomic exchange on its owner, so one of the two loses.
\*  - A hop checks the stop flag and registers as a waiter atomically, so a stop
\*    cannot slip between the check and the wait.
\*  - The snapshot is replaced as one unit, so a read never sees half of it.
\*  - A hop pending when a window opens is served by that window unless the
\*    window's deadline has passed, and a deadline does not starve it forever
\*    (the strong fairness on ServeHop).
EXTENDS Naturals, Sequences

CONSTANTS
  Strict,        \* strict mode: reads hop once the snapshot lacks a write
  MaxCalls,      \* writes and hops the mod issues in all, to keep the model finite
  MaxEvents,     \* non-frame events the engine posts to the mod
  MaxDecisions   \* decisions the engine raises

VARIABLES
  mod,       \* Idle, Running, WaitingHop or Done: the mod thread
  ph,        \* Engine, Applying, Window, Borrowed, Joining or Joined: the game thread
  owner,     \* None, Mod or Game: who holds the store
  stop,      \* a stop was requested
  queue,     \* events waiting for the mod, oldest first: "Frame" or "Ev"
  buf,       \* writes the mod buffered, not yet taken by the game thread
  batch,     \* writes the window took from buf and is applying
  issued,    \* writes and hops in the order the mod made them
  applied,   \* calls the engine has run, in the order it ran them
  snapW,     \* writes the snapshot covers: those applied when it was refreshed
  pend,      \* a decision was raised and is not answered
  posted,    \* this window has posted its Frame
  late,      \* this window's deadline has passed
  staleAns,  \* history: a read was answered from a snapshot lacking a write
  borrows,   \* history: <<mod, phase, queue length>> at each borrow
  nev,       \* events posted so far
  ndec       \* decisions raised so far
vars == <<mod, ph, owner, stop, queue, buf, batch, issued, applied, snapW,
          pend, posted, late, staleAns, borrows, nev, ndec>>

\* Groups of variables an action leaves alone.
calls == <<issued, applied>>
bufs == <<buf, batch>>
clock == <<posted, late>>
hist == <<staleAns, borrows>>
budget == <<nev, ndec>>

Init ==
  /\ mod = "Idle" /\ ph = "Engine" /\ owner = "None" /\ stop = FALSE
  /\ queue = <<>> /\ buf = <<>> /\ batch = <<>>
  /\ issued = <<>> /\ applied = <<>> /\ snapW = 0
  /\ pend = FALSE /\ posted = FALSE /\ late = FALSE
  /\ staleAns = FALSE /\ borrows = {} /\ nev = 0 /\ ndec = 0

IsW(r) == r.k = "W"
IsFrame(e) == e = "Frame"
NotFrame(e) == e # "Frame"
Writes(s) == Len(SelectSeq(s, IsW))

\* A call as the mod issues it: its id is its place in the issued sequence.
Call(k) == [id |-> Len(issued) + 1, k |-> k]
\* A call as the engine runs it, and the thread that ran it.
Perform(r, t) == [id |-> r.id, k |-> r.k, by |-> t]
Performs(s) == [i \in 1..Len(s) |-> Perform(s[i], "Game")]

\* The engine raises a decision, or does not, as a result of a game-thread step.
Raised ==
  \/ pend' = FALSE /\ UNCHANGED ndec
  \/ ndec < MaxDecisions /\ pend' = TRUE /\ ndec' = ndec + 1

\* ---- The mod thread ----

\* Pop: the idle mod takes the oldest event once it holds the store.
Pop ==
  /\ mod = "Idle" /\ ~stop /\ queue # <<>> /\ owner = "None"
  /\ mod' = "Running" /\ owner' = "Mod" /\ queue' = Tail(queue)
  /\ UNCHANGED <<ph, stop, bufs, calls, snapW, pend, clock, hist, budget>>

\* Write: buffered and answered at once.
Write ==
  /\ mod = "Running" /\ Len(issued) < MaxCalls
  /\ issued' = Append(issued, Call("W")) /\ buf' = Append(buf, Call("W"))
  /\ UNCHANGED <<mod, ph, owner, stop, queue, batch, applied, snapW, pend, clock, hist, budget>>

\* ReadSnap: a read answered from the snapshot. Strict mode allows it only
\* while the snapshot covers every write the mod has made, so a read never
\* misses the mod's own write; relaxed mode answers regardless.
ReadSnap ==
  /\ mod = "Running"
  /\ ~Strict \/ Writes(issued) = snapW
  /\ staleAns' = (staleAns \/ Writes(issued) # snapW)
  /\ UNCHANGED <<mod, ph, owner, stop, queue, bufs, calls, snapW, pend, clock, borrows, budget>>

\* Hop: a call the snapshot cannot answer waits for the game thread. After a
\* stop the call fails at once instead, so it never waits.
Hop ==
  /\ mod = "Running" /\ ~stop /\ Len(issued) < MaxCalls
  /\ issued' = Append(issued, Call("H")) /\ mod' = "WaitingHop"
  /\ UNCHANGED <<ph, owner, stop, queue, bufs, applied, snapW, pend, clock, hist, budget>>

\* Finish: the event ends and the mod gives the store up.
Finish ==
  /\ mod = "Running" /\ mod' = "Idle" /\ owner' = "None"
  /\ UNCHANGED <<ph, stop, queue, bufs, calls, snapW, pend, clock, hist, budget>>

\* Trap: a running call reaches its next epoch check after a stop and traps.
Trap ==
  /\ mod = "Running" /\ stop
  /\ mod' = "Done" /\ owner' = "None" /\ queue' = <<>>
  /\ UNCHANGED <<ph, stop, bufs, calls, snapW, pend, clock, hist, budget>>

\* Fail: the mod traps on its own. The game thread joins it at its next tick.
Fail ==
  /\ mod = "Running" /\ ~stop
  /\ mod' = "Done" /\ owner' = "None" /\ queue' = <<>>
  /\ UNCHANGED <<ph, stop, bufs, calls, snapW, pend, clock, hist, budget>>

\* IdleExit: an idle mod thread woken by the stop flag exits.
IdleExit ==
  /\ mod = "Idle" /\ stop
  /\ mod' = "Done" /\ queue' = <<>>
  /\ UNCHANGED <<ph, owner, stop, bufs, calls, snapW, pend, clock, hist, budget>>

\* ---- The game thread, between windows ----

\* PostEv: the engine posts a non-decision event for the mod.
PostEv ==
  /\ ph = "Engine" /\ ~pend /\ mod # "Done" /\ nev < MaxEvents
  /\ queue' = Append(queue, "Ev") /\ nev' = nev + 1
  /\ UNCHANGED <<mod, ph, owner, stop, bufs, calls, snapW, pend, clock, hist, ndec>>

\* Raise: simulation raises a decision that needs an answer now.
Raise ==
  /\ ph = "Engine" /\ ~pend /\ ndec < MaxDecisions
  /\ pend' = TRUE /\ ndec' = ndec + 1
  /\ UNCHANGED <<mod, ph, owner, stop, queue, bufs, calls, snapW, clock, hist, nev>>

\* EndTick: the engine's frame ends and the game thread opens the mod's window.
\* It takes the buffer to apply; a mod writing meanwhile fills a new one.
EndTick ==
  /\ ph = "Engine" /\ ~pend /\ mod # "Done"
  /\ ph' = "Applying" /\ batch' = buf /\ buf' = <<>>
  /\ posted' = FALSE /\ late' = FALSE
  /\ UNCHANGED <<mod, owner, stop, queue, calls, snapW, pend, hist, budget>>

\* ---- The window ----

\* ApplyOne: the game thread runs the oldest buffered write on the engine,
\* which may raise a decision for this same mod.
ApplyOne ==
  /\ ph = "Applying" /\ ~pend /\ batch # <<>>
  /\ applied' = Append(applied, Perform(Head(batch), "Game"))
  /\ batch' = Tail(batch)
  /\ Raised
  /\ UNCHANGED <<mod, ph, owner, stop, queue, buf, issued, snapW, clock, hist, nev>>

\* Refresh: once the buffer is applied, the snapshot is replayed and covers
\* every write applied so far.
Refresh ==
  /\ ph = "Applying" /\ ~pend /\ batch = <<>>
  /\ snapW' = Writes(applied) /\ ph' = "Window"
  /\ UNCHANGED <<mod, owner, stop, queue, bufs, calls, pend, clock, hist, budget>>

\* PostFrame: queue the Frame event. A Frame already queued is replaced.
PostFrame ==
  /\ ph = "Window" /\ ~pend /\ ~posted
  /\ posted' = TRUE
  /\ queue' = IF mod = "Done" THEN queue
              ELSE Append(SelectSeq(queue, NotFrame), "Frame")
  /\ UNCHANGED <<mod, ph, owner, stop, bufs, calls, snapW, pend, late, hist, budget>>

\* ServeHop: the game thread applies the whole buffer, then the hop itself,
\* and answers the waiting mod.
ServeHop ==
  /\ ph = "Window" /\ ~pend /\ mod = "WaitingHop"
  /\ applied' = applied \o Performs(buf)
                \o <<Perform(issued[Len(issued)], "Game")>>
  /\ buf' = <<>> /\ mod' = "Running"
  /\ Raised
  /\ UNCHANGED <<ph, owner, stop, queue, batch, issued, snapW, clock, hist, nev>>

\* Close: the window ends once the mod is quiet or the deadline passed. A hop
\* still waiting then is served by the next window.
Close ==
  /\ ph = "Window" /\ ~pend /\ posted
  /\ late \/ (mod \in {"Idle", "Done"} /\ queue = <<>>)
  /\ ph' = "Engine" /\ posted' = FALSE /\ late' = FALSE
  /\ UNCHANGED <<mod, owner, stop, queue, bufs, calls, snapW, pend, hist, budget>>

\* Late: the window's deadline passes. Time is the environment's.
Late ==
  /\ ph \in {"Applying", "Window"} /\ ~late /\ late' = TRUE
  /\ UNCHANGED <<mod, ph, owner, stop, queue, bufs, calls, snapW, pend, posted, hist, budget>>

\* ---- Decisions ----

\* CanBorrow: the game thread may run a decision on the store in place only
\* between windows, when the mod is idle with no event queued ahead of the
\* decision, and the store is free to claim.
CanBorrow ==
  ph = "Engine" /\ mod = "Idle" /\ owner = "None" /\ queue = <<>>

\* Borrow: claim the store, apply the buffer so the decision sees the mod's
\* writes, and deliver the decision in place.
Borrow ==
  /\ pend /\ CanBorrow
  /\ owner' = "Game" /\ ph' = "Borrowed" /\ pend' = FALSE
  /\ applied' = applied \o Performs(batch \o buf)
  /\ buf' = <<>> /\ batch' = <<>>
  /\ borrows' = borrows \union {<<mod, ph, Len(queue)>>}
  /\ UNCHANGED <<mod, stop, queue, issued, snapW, clock, staleAns, budget>>

\* Default: the mod cannot answer now, so the decision takes its default
\* answer. The game thread does not wait for the mod.
Default ==
  /\ pend /\ ~CanBorrow
  /\ pend' = FALSE
  /\ UNCHANGED <<mod, ph, owner, stop, queue, bufs, calls, snapW, clock, hist, budget>>

\* Direct: the borrowed decision's handler makes a call, which runs directly.
\* It may raise a decision for the mod it is running in; that one defaults.
Direct ==
  /\ ph = "Borrowed" /\ ~pend /\ Len(issued) < MaxCalls
  /\ issued' = Append(issued, Call("W"))
  /\ applied' = Append(applied, Perform(Call("W"), "Game"))
  /\ Raised
  /\ UNCHANGED <<mod, ph, owner, stop, queue, bufs, snapW, clock, hist, nev>>

\* EndBorrow: the decision is delivered; the store goes back to the mod.
EndBorrow ==
  /\ ph = "Borrowed" /\ ~pend
  /\ ph' = "Engine" /\ owner' = "None"
  /\ UNCHANGED <<mod, stop, queue, bufs, calls, snapW, pend, clock, hist, budget>>

\* ---- Stop ----

\* Stop: the game thread sets the flag and joins the mod thread.
Stop ==
  /\ ph = "Engine" /\ ~pend /\ ~stop
  /\ stop' = TRUE /\ ph' = "Joining"
  /\ UNCHANGED <<mod, owner, queue, bufs, calls, snapW, pend, clock, hist, budget>>

\* ObserveDead: a mod that failed on its own is joined at the next tick.
ObserveDead ==
  /\ ph = "Engine" /\ ~pend /\ mod = "Done"
  /\ ph' = "Joining"
  /\ UNCHANGED <<mod, owner, stop, queue, bufs, calls, snapW, pend, clock, hist, budget>>

\* WakeWaiter: the joining game thread cannot serve hops, so it answers a
\* waiting hop with an error. The mod then traps at its next epoch check.
WakeWaiter ==
  /\ ph = "Joining" /\ mod = "WaitingHop"
  /\ mod' = "Running"
  /\ UNCHANGED <<ph, owner, stop, queue, bufs, calls, snapW, pend, clock, hist, budget>>

\* Join: the thread has exited; the game thread drops the store.
Join ==
  /\ ph = "Joining" /\ mod = "Done"
  /\ ph' = "Joined"
  /\ UNCHANGED <<mod, owner, stop, queue, bufs, calls, snapW, pend, clock, hist, budget>>

Next == Pop \/ Write \/ ReadSnap \/ Hop \/ Finish \/ Trap \/ Fail \/ IdleExit
        \/ PostEv \/ Raise \/ EndTick \/ ApplyOne \/ Refresh \/ PostFrame
        \/ ServeHop \/ Close \/ Late \/ Borrow \/ Default \/ Direct
        \/ EndBorrow \/ Stop \/ ObserveDead \/ WakeWaiter \/ Join
        \/ UNCHANGED vars

\* The game thread, the thread scheduler and the clock make progress. The mod's
\* own code need not: it may loop forever, so Write, ReadSnap, Hop, Finish and
\* Fail carry no fairness, and the properties below hold with a mod that never
\* returns. Trap is the epoch ticker, which does not depend on the mod.
Fair ==
  /\ WF_vars(EndTick) /\ WF_vars(ApplyOne) /\ WF_vars(Refresh)
  /\ WF_vars(PostFrame) /\ WF_vars(Close) /\ WF_vars(Late)
  /\ WF_vars(Borrow) /\ WF_vars(Default) /\ WF_vars(EndBorrow)
  /\ WF_vars(ObserveDead) /\ WF_vars(WakeWaiter) /\ WF_vars(Join)
  /\ WF_vars(Pop) /\ WF_vars(IdleExit) /\ WF_vars(Trap)
  /\ SF_vars(ServeHop)
Spec == Init /\ [][Next]_vars /\ Fair

\* ---- Properties ----

TypeOK ==
  /\ mod \in {"Idle", "Running", "WaitingHop", "Done"}
  /\ ph \in {"Engine", "Applying", "Window", "Borrowed", "Joining", "Joined"}
  /\ owner \in {"None", "Mod", "Game"}
  /\ Len(issued) <= MaxCalls
  /\ \A i \in 1..Len(issued) : issued[i].id = i
  /\ pend \in BOOLEAN /\ posted \in BOOLEAN /\ late \in BOOLEAN

\* The store is used by one thread at a time: the mod holds it from the pop to
\* the end of the event, including while it waits on a hop, and the game thread
\* only while it runs a borrowed decision.
StoreExclusive ==
  /\ ~(mod = "Running" /\ ph = "Borrowed")
  /\ (ph = "Borrowed" => (owner = "Game" /\ mod = "Idle"))
  /\ (mod \in {"Running", "WaitingHop"} => owner = "Mod")

\* Every engine call runs on the game thread.
EngineOnGame == \A i \in 1..Len(applied) : applied[i].by = "Game"

\* The engine sees the calls in the order the mod made them, and none the mod
\* did not make: what ran is a prefix of what was issued.
WriteOrder ==
  /\ Len(applied) <= Len(issued)
  /\ \A i \in 1..Len(applied) :
       applied[i].id = i /\ applied[i].k = issued[i].k

\* In strict mode no read is answered from a snapshot that lacks a write the
\* mod already made, in any earlier event too.
StrictReads == Strict => ~staleAns

\* A decision borrows the store only between windows, with the mod idle and
\* no event queued ahead of it; otherwise it takes the default answer.
DecisionBorrow == borrows \subseteq {<<"Idle", "Engine", 0>>}

\* At most one Frame waits in the queue.
QueueCoalesced == Len(SelectSeq(queue, IsFrame)) <= 1

\* A decision is answered without the mod's help: this holds although the mod
\* is not fair.
DecisionAnswered == pend ~> ~pend

\* A hop is answered, by a window or by the stop.
HopAnswered == mod = "WaitingHop" ~> mod # "WaitingHop"

\* A stop ends with the thread joined, and so does a mod that failed.
StopJoins == stop ~> ph = "Joined"
FailJoins == mod = "Done" ~> ph = "Joined"

\* Every window ends, whatever the mod does.
WindowEnds == ph \in {"Applying", "Window"} ~> ph \notin {"Applying", "Window"}
====
