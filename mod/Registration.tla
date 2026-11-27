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
          savedGeneration
vars == <<phase, handlers, subscription, command, captured, queue, delivered,
          expected, commandAtQueue, pending, completions, replyPhase, tenure, kept, generation,
          savedGeneration>>
Init == /\ phase = "init" /\ handlers = <<1>> /\ subscription = FALSE
        /\ command = 1 /\ captured = 0 /\ queue = "none"
        /\ delivered = <<>> /\ expected = <<>> /\ commandAtQueue = 0
        /\ pending = TRUE /\ completions = 0 /\ replyPhase = "ready"
        /\ tenure = 1 /\ kept = 1 /\ generation = 1 /\ savedGeneration = 0
Start == /\ phase = "init" /\ phase' = "running"
         \* A Start callback registers the second handler before subscriptions.
         /\ handlers' = Append(handlers, 2) /\ subscription' = TRUE
         /\ UNCHANGED <<commandAtQueue, command, captured, queue, delivered, expected, pending,
                        completions, replyPhase, tenure, kept, generation, savedGeneration>>
Queue == /\ phase = "running" /\ queue = "none"
         /\ queue' = "waiting" /\ captured' = command /\ commandAtQueue' = command
         /\ UNCHANGED <<phase, handlers, subscription, command, delivered, expected,
                        pending, completions, replyPhase, tenure, kept, generation, savedGeneration>>
RegisterLate == /\ phase = "running" /\ Len(handlers) = 2
                /\ handlers' = Append(handlers, 3) /\ command' = 2
                /\ UNCHANGED <<commandAtQueue, phase, subscription, captured, queue, delivered, expected,
                               pending, completions, replyPhase, tenure, kept, generation, savedGeneration>>
Frame == /\ queue = "waiting" /\ queue' = "done"
         \* Timers precede frame callbacks. The list is read here; command is captured.
         /\ delivered' = <<captured>> \o handlers \o handlers
         /\ expected' = <<captured>> \o handlers \o handlers
         /\ UNCHANGED <<commandAtQueue, phase, handlers, subscription, command, captured, pending,
                        completions, replyPhase, tenure, kept, generation, savedGeneration>>
BeginReply == /\ replyPhase = "ready" /\ replyPhase' = "callback"
              /\ pending' = FALSE
              /\ completions' = completions + 1
              /\ UNCHANGED <<commandAtQueue, phase, handlers, subscription, command, captured, queue,
                             delivered, expected, tenure, kept, generation, savedGeneration>>
DuplicateReply == /\ replyPhase = "callback" /\ replyPhase' = "done"
                  /\ completions' = IF pending THEN completions + 1 ELSE completions
                  /\ pending' = FALSE
                  /\ UNCHANGED <<commandAtQueue, phase, handlers, subscription, command, captured, queue,
                                 delivered, expected, tenure, kept, generation, savedGeneration>>
ReplaceConnection == /\ generation = 1 /\ generation' = 2
                     /\ tenure' = tenure + 1
                     /\ UNCHANGED <<commandAtQueue, phase, handlers, subscription, command, captured, queue,
                                    delivered, expected, pending, completions, replyPhase, kept, savedGeneration>>
Persist == /\ generation = 2 /\ savedGeneration = 0
           /\ savedGeneration' = IF kept = tenure THEN generation ELSE 0
           /\ UNCHANGED <<commandAtQueue, phase, handlers, subscription, command, captured, queue,
                          delivered, expected, pending, completions, replyPhase, tenure, kept, generation>>
Done == /\ phase = "running" /\ queue = "done" /\ replyPhase = "done"
        /\ generation = 2 /\ UNCHANGED vars
Next == Start \/ Queue \/ RegisterLate \/ Frame \/ BeginReply \/ DuplicateReply
        \/ ReplaceConnection \/ Persist \/ Done
Spec == Init /\ [][Next]_vars
SubscriptionsAfterStart == phase = "running" => subscription /\ Len(handlers) >= 2
CallbackOrder == delivered = expected
CommandCapture == queue = "done" => Head(delivered) = commandAtQueue
AtMostOnce == completions <= 1
SavedConnection == savedGeneration = 0
====
