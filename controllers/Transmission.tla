---- MODULE Transmission ----
\* Device construction commits its edge-free baseline before send. Send failure
\* ends the whole Run, so a later Run starts a fresh stream. Reader completion
\* is buffered once; stop must drain it before another start can own the slot.
EXTENDS Naturals
VARIABLES stateEpoch, run, live, completion, starts, stops, announced, edges, delivered, sent, epoch, emittedEpoch, frameEdges, fullFrame
vars == <<stateEpoch, run, live, completion, starts, stops, announced, edges, delivered, sent, epoch, emittedEpoch, frameEdges, fullFrame>>
Init == /\ stateEpoch = 0 /\ run = "idle" /\ live = 0 /\ completion = FALSE /\ starts = 0 /\ stops = 0
        /\ announced = FALSE /\ edges = 0 /\ delivered = 0 /\ sent = FALSE
        /\ epoch = 1 /\ emittedEpoch = 0 /\ frameEdges = 0 /\ fullFrame = FALSE
Start == /\ run = "idle" /\ ~completion /\ starts < 2
         /\ run' = "running" /\ live' = live + 1 /\ starts' = starts + 1
         /\ UNCHANGED <<stateEpoch, completion, stops, announced, edges, delivered, sent, epoch, emittedEpoch, frameEdges, fullFrame>>
Cancel == /\ run = "running" /\ run' = "stopping"
          /\ UNCHANGED <<stateEpoch, live, completion, starts, stops, announced, edges, delivered, sent, epoch, emittedEpoch, frameEdges, fullFrame>>
Complete == /\ run \in {"running", "stopping"} /\ ~completion
            /\ live' = live - 1 /\ completion' = TRUE
            /\ UNCHANGED <<stateEpoch, run, starts, stops, announced, edges, delivered, sent, epoch, emittedEpoch, frameEdges, fullFrame>>
Drain == /\ completion /\ run' = "idle" /\ completion' = FALSE /\ stops' = stops + 1
         /\ UNCHANGED <<stateEpoch, live, starts, announced, edges, delivered, sent, epoch, emittedEpoch, frameEdges, fullFrame>>
Edge == /\ edges = 0 /\ delivered = 0 /\ edges' = 1
        /\ UNCHANGED <<stateEpoch, run, live, completion, starts, stops, announced, delivered, sent, epoch, emittedEpoch, frameEdges, fullFrame>>
Frame == /\ emittedEpoch = epoch
         /\ announced' = TRUE /\ sent' = TRUE
         /\ frameEdges' = edges /\ fullFrame' = FALSE
         /\ delivered' = delivered + edges /\ edges' = 0
         /\ UNCHANGED <<stateEpoch, run, live, completion, starts, stops, epoch, emittedEpoch>>
Full == /\ emittedEpoch # epoch /\ emittedEpoch' = epoch /\ stateEpoch' = epoch
        /\ announced' = TRUE /\ sent' = TRUE /\ edges' = 0
        /\ frameEdges' = 0 /\ fullFrame' = TRUE
        /\ UNCHANGED <<run, live, completion, starts, stops, delivered, epoch>>
NewEpoch == /\ epoch = 1 /\ emittedEpoch = epoch /\ epoch' = 2
            /\ UNCHANGED <<stateEpoch, run, live, completion, starts, stops, announced, edges, delivered, sent, emittedEpoch, frameEdges, fullFrame>>
Next == Start \/ Cancel \/ Complete \/ Drain \/ Edge \/ Frame \/ Full \/ NewEpoch
Spec == Init /\ [][Next]_vars
OneRun == live <= 1 /\ (run = "idle" => live = 0 /\ ~completion)
NoEarlyRestart == starts <= stops + 1
EdgesOnce == delivered <= 1
FullBaseline == emittedEpoch = epoch => announced /\ sent /\ stateEpoch = epoch
EpochReset == fullFrame => frameEdges = 0
====
