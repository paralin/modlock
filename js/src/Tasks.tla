---- MODULE Tasks ----
\* The tasks a mod runs across frames, as tasks.ts schedules them. A task is
\* an async function that does its work in steps and calls pace between them.
\* Each frame the library runs the mod's frame handlers, then resumes every
\* task that waited for that frame, in the order they began waiting. pace
\* returns at once while the frame is within its budget and waits for the next
\* frame once it is past it; a budget of zero, which the host sends when it
\* gives the mod none, never runs out. cancel rejects the task's next pace or
\* frame, so the task runs no further step and its finally blocks unwind.
\*
\* Between two steps the mod's event has returned to the host, so a damage,
\* command or ping event in a later tick runs at once against live state; that
\* is why tasks yield instead of the host preempting a call, and why it needs
\* no model here.
\*
\* One unit of cost is one step. Steps are not split, so a frame can overrun
\* its budget by the step each resumed task runs: the cost of guaranteeing
\* every task a step in each frame that resumes it. Tasks.mutants.json holds
\* the rejected alternative in which a resumed task checks the clock before its
\* first step; a task behind a costly one then never runs.
EXTENDS Naturals, Sequences, FiniteSets
CONSTANTS Tasks, Steps, Budget, Frames
VARIABLES left, state, waiting, batch, used, frame, ran, phase
vars == <<left, state, waiting, batch, used, frame, ran, phase>>

States == {"waiting", "done", "cancelled"}
Late == Budget # 0 /\ used >= Budget

Init ==
  /\ left = [t \in Tasks |-> Steps]
  /\ state = [t \in Tasks |-> "waiting"]
  \* Every task began on the frame before the first, in a fixed order.
  /\ waiting \in {s \in [1..Cardinality(Tasks) -> Tasks] :
                    \A i, j \in 1..Cardinality(Tasks) : i # j => s[i] # s[j]}
  /\ batch = <<>>
  /\ used = 0
  /\ frame = 0
  /\ ran = [t \in Tasks |-> 0]
  /\ phase = "idle"

\* BeginFrame resumes the tasks that waited for this frame, after the frame
\* handlers spent some of the budget.
BeginFrame ==
  /\ phase = "idle" /\ frame < Frames
  /\ \E handlers \in 0..1 :
       used' = handlers
  /\ batch' = waiting
  /\ waiting' = <<>>
  /\ frame' = frame + 1
  /\ ran' = [t \in Tasks |-> 0]
  /\ phase' = "tasks"
  /\ UNCHANGED <<left, state>>

\* Cancelled marks a task the mod cancelled; it can happen at any time.
Cancel(t) ==
  /\ state[t] = "waiting"
  /\ state' = [state EXCEPT ![t] = "cancelled"]
  /\ UNCHANGED <<left, waiting, batch, used, frame, ran, phase>>

\* Resume runs one step of the batch's head, then paces it: a finished task
\* leaves, a task that finds the frame late waits for the next one, and any
\* other task takes its next turn after the rest of the batch. A cancelled
\* task's wait rejects and the task ends without a step.
Resume ==
  /\ phase = "tasks" /\ batch # <<>>
  /\ LET t == Head(batch) IN
     IF state[t] = "cancelled" THEN
       /\ batch' = Tail(batch)
       /\ UNCHANGED <<left, state, waiting, used, frame, ran, phase>>
     ELSE
       \* Run one step, then pace: run on while within budget.
       /\ left' = [left EXCEPT ![t] = @ - 1]
       /\ used' = used + 1
       /\ ran' = [ran EXCEPT ![t] = @ + 1]
       /\ IF left[t] = 1 THEN
            /\ state' = [state EXCEPT ![t] = "done"]
            /\ batch' = Tail(batch)
            /\ UNCHANGED waiting
          ELSE IF Budget # 0 /\ used + 1 >= Budget THEN
            /\ batch' = Tail(batch)
            /\ waiting' = Append(waiting, t)
            /\ UNCHANGED state
          ELSE
            /\ batch' = Append(Tail(batch), t)
            /\ UNCHANGED <<state, waiting>>
       /\ UNCHANGED <<frame, phase>>

EndFrame ==
  /\ phase = "tasks" /\ batch = <<>>
  /\ phase' = "idle"
  /\ UNCHANGED <<left, state, waiting, batch, used, frame, ran>>

Next ==
  \/ BeginFrame \/ Resume \/ EndFrame
  \/ \E t \in Tasks : Cancel(t)

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ state \in [Tasks -> States]
  /\ left \in [Tasks -> 0..Steps]

\* A cancelled task runs no step after its cancel: its steps stay put.
CancelledStops == [][\A t \in Tasks : state[t] = "cancelled" => left'[t] = left[t]]_vars

\* Every live task resumed in a frame ran a step in it, so a costly task cannot
\* starve the ones behind it.
EachTaskSteps ==
  (phase = "idle" /\ frame > 0) =>
    \A t \in Tasks : state[t] = "waiting" => ran[t] >= 1

\* With no budget, pace never waits, so a task ends in the frame it resumes.
NoBudgetIsSynchronous ==
  (Budget = 0 /\ phase = "idle" /\ frame > 0) =>
    \A t \in Tasks : state[t] # "waiting"

\* A frame overruns its budget by at most one step for each task.
OverrunBounded ==
  Budget # 0 => used <= Budget + Cardinality(Tasks)
====
