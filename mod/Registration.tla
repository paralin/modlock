---- MODULE Registration ----
\* The serial Go event boundary: init and Start register callbacks; Start
\* derives subscriptions after its callbacks. Command captures its handler
\* before After, while chat and UI read their lists when After runs. A range
\* sees its initial length, even when a callback registers another callback.
\* serveMod, seating's late OnFrame, service completions (including callback
\* reentry), and a kept Player used by persist are modeled below. Timers and
\* spawn internals have their own code contracts; ABI encoding is checked by
\* the Wasm integration test. There are no concurrent Go callers or locks.
\* The mixed handlers baseline and Registry/adapter candidate implement these
\* same transitions: splitting ownership adds no lifecycle state or transfer.
EXTENDS Naturals, Sequences
VARIABLES phase, handlers, subscription, command, captured, queue, delivered,
          expected, commandAtQueue, pending, completions, replyPhase, tenure, kept, generation,
          savedGeneration, refusedPhase, refusedReturned, refusedCompletions,
          snapshot, cursor, duringAdded, timersDone
vars == <<phase, handlers, subscription, command, captured, queue, delivered,
          expected, commandAtQueue, pending, completions, replyPhase, tenure, kept, generation,
          savedGeneration, refusedPhase, refusedReturned, refusedCompletions,
          snapshot, cursor, duringAdded, timersDone>>
Init == /\ phase = "init" /\ handlers = <<1>> /\ subscription = FALSE
        /\ command = 1 /\ captured = 0 /\ queue = "none"
        /\ delivered = <<>> /\ expected = <<>> /\ commandAtQueue = 0
        /\ pending = TRUE /\ completions = 0 /\ replyPhase = "ready"
        /\ tenure = 1 /\ kept = 1 /\ generation = 1 /\ savedGeneration = 0
        /\ refusedPhase = "idle" /\ refusedReturned = FALSE /\ refusedCompletions = 0
        /\ snapshot = <<>> /\ cursor = 1 /\ duringAdded = FALSE /\ timersDone = FALSE
Start == /\ phase = "init" /\ phase' = "running"
         \* A Start callback registers the second handler before subscriptions.
         /\ handlers' = Append(handlers, 2) /\ subscription' = TRUE
         /\ UNCHANGED <<snapshot, cursor, duringAdded, timersDone, refusedPhase, refusedReturned, refusedCompletions, commandAtQueue, command, captured, queue, delivered, expected, pending,
                        completions, replyPhase, tenure, kept, generation, savedGeneration>>
Queue == /\ phase = "running" /\ queue = "none"
         /\ queue' = "waiting" /\ captured' = command /\ commandAtQueue' = command
         /\ UNCHANGED <<snapshot, cursor, duringAdded, timersDone, refusedPhase, refusedReturned, refusedCompletions, phase, handlers, subscription, command, delivered, expected,
                        pending, completions, replyPhase, tenure, kept, generation, savedGeneration>>
RegisterLate == /\ phase = "running" /\ Len(handlers) = 2
                /\ handlers' = Append(handlers, 3) /\ command' = 2
                /\ UNCHANGED <<snapshot, cursor, duringAdded, timersDone, refusedPhase, refusedReturned, refusedCompletions, commandAtQueue, phase, subscription, captured, queue, delivered, expected,
                               pending, completions, replyPhase, tenure, kept, generation, savedGeneration>>
\* Begin each list at its actual delivery time. The same abstract list stands
\* for chat, UI and frame registrations; the callback tags distinguish their
\* order. Late registration can occur before or during any range.
Running(kind) == CASE kind = "chat" -> "chatRunning"
                    [] kind = "ui" -> "uiRunning"
                    [] OTHER -> "frameRunning"
Tagged(kind, entries) == [i \in 1..Len(entries) |-> [kind |-> kind, id |-> entries[i]]]
CommandFrame == /\ queue = "waiting" /\ queue' = "chat"
                /\ delivered' = Append(delivered, [kind |-> "command", id |-> captured])
                /\ expected' = Append(expected, [kind |-> "command", id |-> commandAtQueue])
                /\ UNCHANGED <<snapshot, cursor, duringAdded, timersDone,
                               phase, handlers, subscription, command, captured, commandAtQueue,
                               pending, completions, replyPhase, tenure, kept, generation,
                               savedGeneration, refusedPhase, refusedReturned, refusedCompletions>>
BeginList(kind) == /\ queue = kind /\ queue' = Running(kind)
                   /\ snapshot' = handlers /\ cursor' = 1
                   /\ expected' = expected \o Tagged(kind, handlers)
                   /\ UNCHANGED <<duringAdded, timersDone, delivered,
                                  phase, handlers, subscription, command, captured, commandAtQueue,
                                  pending, completions, replyPhase, tenure, kept, generation,
                                  savedGeneration, refusedPhase, refusedReturned, refusedCompletions>>
Frame == /\ queue = "frame" /\ timersDone /\ BeginList("frame")
Deliver(kind) == /\ queue = Running(kind) /\ cursor <= Len(snapshot)
                 /\ delivered' = Append(delivered, [kind |-> kind, id |-> snapshot[cursor]])
                 /\ cursor' = cursor + 1
                 /\ UNCHANGED <<snapshot, duringAdded, timersDone, expected, queue,
                                phase, handlers, subscription, command, captured, commandAtQueue,
                                pending, completions, replyPhase, tenure, kept, generation,
                                savedGeneration, refusedPhase, refusedReturned, refusedCompletions>>
EndList(kind, next) == /\ queue = Running(kind) /\ cursor > Len(snapshot)
                       /\ queue' = next
                       /\ timersDone' = (timersDone \/ kind = "ui")
                       /\ UNCHANGED <<snapshot, cursor, duringAdded, delivered, expected,
                                      phase, handlers, subscription, command, captured, commandAtQueue,
                                      pending, completions, replyPhase, tenure, kept, generation,
                                      savedGeneration, refusedPhase, refusedReturned, refusedCompletions>>
RegisterDuringDelivery == /\ queue \in {"chatRunning", "uiRunning", "frameRunning"}
                          /\ cursor = 2 /\ ~duringAdded
                          /\ handlers' = Append(handlers, 4) /\ duringAdded' = TRUE
                          /\ UNCHANGED <<snapshot, cursor, timersDone, delivered, expected, queue,
                                         phase, subscription, command, captured, commandAtQueue,
                                         pending, completions, replyPhase, tenure, kept, generation,
                                         savedGeneration, refusedPhase, refusedReturned, refusedCompletions>>
BeginReply == /\ replyPhase = "ready" /\ replyPhase' = "callback"
              /\ pending' = FALSE
              /\ completions' = completions + 1
              /\ UNCHANGED <<snapshot, cursor, duringAdded, timersDone, refusedPhase, refusedReturned, refusedCompletions, commandAtQueue, phase, handlers, subscription, command, captured, queue,
                             delivered, expected, tenure, kept, generation, savedGeneration>>
DuplicateReply == /\ replyPhase = "callback" /\ replyPhase' = "done"
                  /\ completions' = IF pending THEN completions + 1 ELSE completions
                  /\ pending' = FALSE
                  /\ UNCHANGED <<snapshot, cursor, duringAdded, timersDone, refusedPhase, refusedReturned, refusedCompletions, commandAtQueue, phase, handlers, subscription, command, captured, queue,
                                 delivered, expected, tenure, kept, generation, savedGeneration>>
ReplaceConnection == /\ generation = 1 /\ generation' = 2
                     /\ tenure' = tenure + 1
                     /\ UNCHANGED <<snapshot, cursor, duringAdded, timersDone, refusedPhase, refusedReturned, refusedCompletions, commandAtQueue, phase, handlers, subscription, command, captured, queue,
                                    delivered, expected, pending, completions, replyPhase, kept, savedGeneration>>
Persist == /\ generation = 2 /\ savedGeneration = 0
           /\ savedGeneration' = IF kept = tenure THEN generation ELSE 0
           /\ UNCHANGED <<snapshot, cursor, duringAdded, timersDone, refusedPhase, refusedReturned, refusedCompletions, commandAtQueue, phase, handlers, subscription, command, captured, queue,
                          delivered, expected, pending, completions, replyPhase, tenure, kept, generation>>
\* A refused host start schedules its failure; After cannot enter the module
\* until CallServiceLater has returned to the serial host boundary.
RefuseStart == /\ refusedPhase = "idle" /\ refusedPhase' = "scheduled"
               /\ UNCHANGED <<snapshot, cursor, duringAdded, timersDone, phase, handlers, subscription, command, captured, queue, delivered,
                              expected, commandAtQueue, pending, completions, replyPhase, tenure,
                              kept, generation, savedGeneration, refusedReturned, refusedCompletions>>
ReturnStart == /\ refusedPhase = "scheduled" /\ ~refusedReturned /\ refusedReturned' = TRUE
               /\ UNCHANGED <<snapshot, cursor, duringAdded, timersDone, phase, handlers, subscription, command, captured, queue, delivered,
                              expected, commandAtQueue, pending, completions, replyPhase, tenure,
                              kept, generation, savedGeneration, refusedPhase, refusedCompletions>>
FailureFrame == /\ refusedPhase = "scheduled" /\ refusedReturned
                /\ refusedPhase' = "done" /\ refusedCompletions' = 1
                /\ UNCHANGED <<snapshot, cursor, duringAdded, timersDone, phase, handlers, subscription, command, captured, queue, delivered,
                               expected, commandAtQueue, pending, completions, replyPhase, tenure,
                               kept, generation, savedGeneration, refusedReturned>>
Done == /\ phase = "running" /\ queue = "done" /\ replyPhase = "done"
        /\ generation = 2 /\ refusedPhase = "done" /\ UNCHANGED vars
Next == Start \/ Queue \/ RegisterLate \/ RegisterDuringDelivery \/ CommandFrame
        \/ BeginList("chat") \/ BeginList("ui") \/ Frame
        \/ (\E kind \in {"chat", "ui", "frame"} : Deliver(kind))
        \/ EndList("chat", "ui") \/ EndList("ui", "frame") \/ EndList("frame", "done")
        \/ BeginReply \/ DuplicateReply
        \/ ReplaceConnection \/ Persist \/ RefuseStart \/ ReturnStart \/ FailureFrame \/ Done
Spec == Init /\ [][Next]_vars
SubscriptionsAfterStart == phase = "running" => subscription /\ Len(handlers) >= 2
CallbackOrder == /\ Len(delivered) <= Len(expected)
                 /\ delivered = SubSeq(expected, 1, Len(delivered))
                 /\ (queue = "done" => delivered = expected)
CommandCapture == Len(delivered) > 0 => Head(delivered).id = commandAtQueue
TimersBeforeFrames == queue \in {"frameRunning", "done"} => timersDone
AtMostOnce == completions <= 1
SavedConnection == savedGeneration = 0
NoPrematureCompletion == refusedCompletions = 1 => refusedReturned
====
