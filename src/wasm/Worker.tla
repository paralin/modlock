---- MODULE Worker ----
\* A mod's worker, as worker.cc runs it. The engine thread posts requests and
\* at last destroys the worker; the worker's thread loads its store, then
\* answers the requests one at a time. Each request's reply must end exactly
\* once, whether the worker answers it, fails before or while running it, or
\* stops. Post and Fail exchange the queue under one mutex, so each is one
\* step here.
\*
\* A request runs for Long epoch ticks. The destroyer requests a stop and
\* waits for the thread; with Design "interrupt", the running request's store
\* traps on the next tick, so the wait is one tick. Design "wait" is the
\* probe's, in which the stop waits for the request to finish: its mutant
\* fails StopIsPrompt.
EXTENDS Naturals, Sequences, FiniteSets
CONSTANTS Requests, Long, Design
VARIABLES worker, queue, running, left, replies, posted, stopping, waited
vars == <<worker, queue, running, left, replies, posted, stopping, waited>>

None == "none"
Workers == {"loading", "ready", "running", "failed", "exited", "destroyed"}

Init ==
  /\ worker = "loading"
  /\ queue = <<>>
  /\ running = None
  /\ left = 0
  /\ replies = [r \in Requests |-> 0]
  /\ posted = {}
  /\ stopping = FALSE
  /\ waited = 0

Reply(r) == replies' = [replies EXCEPT ![r] = @ + 1]
ReplyAll(s) == replies' = [r \in Requests |->
                 IF \E i \in 1..Len(s) : s[i] = r THEN replies[r] + 1 ELSE replies[r]]

\* Post, on the engine thread: refused at once by a failed worker, else
\* queued.
Post(r) ==
  /\ ~stopping /\ r \notin posted
  /\ posted' = posted \cup {r}
  /\ IF worker = "failed"
       THEN Reply(r) /\ UNCHANGED queue
       ELSE queue' = Append(queue, r) /\ UNCHANGED replies
  /\ UNCHANGED <<worker, running, left, stopping, waited>>

\* The store loads, or fails to and refuses everything queued.
Loaded ==
  /\ worker = "loading" /\ ~stopping
  /\ \/ worker' = "ready" /\ UNCHANGED <<queue, replies>>
     \/ worker' = "failed" /\ ReplyAll(queue) /\ queue' = <<>>
  /\ UNCHANGED <<running, left, posted, stopping, waited>>

\* The thread takes the next request, unless a stop was requested.
Take ==
  /\ worker = "ready" /\ queue # <<>> /\ ~stopping
  /\ running' = Head(queue) /\ queue' = Tail(queue)
  /\ left' = Long /\ worker' = "running"
  /\ UNCHANGED <<replies, posted, stopping, waited>>

\* One epoch tick of the running request.
Tick ==
  /\ worker = "running"
  /\ waited' = IF stopping THEN waited + 1 ELSE waited
  /\ IF stopping /\ Design = "interrupt" THEN
       \* The store traps: the request ends with an error and the thread
       \* exits.
       /\ Reply(running) /\ running' = None /\ worker' = "exited"
       /\ UNCHANGED <<queue, left>>
     ELSE IF left = 1 THEN
       \* The request ends, answered or trapped; a trap fails the worker,
       \* which refuses what waits.
       /\ running' = None /\ left' = 0
       /\ \/ /\ worker' = (IF stopping THEN "exited" ELSE "ready")
             /\ Reply(running) /\ UNCHANGED queue
          \/ /\ ~stopping /\ worker' = "failed" /\ queue' = <<>>
             /\ replies' = [r \in Requests |->
                  replies[r] + (IF r = running THEN 1 ELSE 0)
                  + (IF \E i \in 1..Len(queue) : queue[i] = r THEN 1 ELSE 0)]
     ELSE
       /\ left' = left - 1
       /\ UNCHANGED <<replies, running, worker, queue>>
  /\ UNCHANGED <<posted, stopping>>

\* The destroyer requests the stop; an idle or failed thread exits at once.
Stop ==
  /\ ~stopping
  /\ stopping' = TRUE
  /\ worker' = IF worker = "running" THEN "running" ELSE "exited"
  /\ UNCHANGED <<queue, running, left, replies, posted, waited>>

\* The join returns, and the destructor refuses what still waits.
Destroy ==
  /\ worker = "exited"
  /\ worker' = "destroyed"
  /\ ReplyAll(queue) /\ queue' = <<>>
  /\ UNCHANGED <<running, left, posted, stopping, waited>>

Next ==
  \/ \E r \in Requests : Post(r)
  \/ Loaded \/ Take \/ Tick \/ Stop \/ Destroy

Spec == Init /\ [][Next]_vars /\ WF_vars(Loaded \/ Take \/ Tick \/ Destroy) /\ WF_vars(Stop)

TypeOK ==
  /\ worker \in Workers
  /\ replies \in [Requests -> 0..2]

\* No reply ends twice.
ReplyOnce == \A r \in Requests : replies[r] <= 1

\* A destroyed worker leaves no posted request without its reply.
DestroyedAnswered == worker = "destroyed" => \A r \in posted : replies[r] = 1

\* The stop waits at most one tick for the running request.
StopIsPrompt == waited <= 1

\* Every posted request is answered.
Answered == \A r \in Requests : r \in posted ~> replies[r] = 1
====
