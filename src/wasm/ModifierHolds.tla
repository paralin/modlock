---- MODULE ModifierHolds ----
\* ModifierHolds::HoldModifier and HoldEntityModifier attach one definition
\* per held target; Apply restores a lost modifier. Clear, called by a stop,
\* reload or plugin failure, marks matching definitions on held targets for native removal and
\* restores their definition durations. The engine finishes removal later.
\* WorldEnding forgets holds because the world itself destroys all entities.
\* One-shot GiveModifier calls and explicit release (which ends maintenance
\* without removing the modifier) are outside this stop-cleanup contract.
EXTENDS Naturals, FiniteSets
Pairs == {"hero", "entity"} \X {"standalone", "embedded"}
VARIABLES running, cleanup, holds, active, pending, stopped
vars == <<running, cleanup, holds, active, pending, stopped>>

Init == /\ running = TRUE /\ cleanup \in BOOLEAN
        /\ holds = {} /\ active = {} /\ pending = {} /\ stopped = {}

\* Admit an endless hold only after its native cleanup call resolves.
CanHold == cleanup
Hold(p) == /\ running /\ CanHold
           /\ holds' = holds \cup {p} /\ active' = active \cup {p}
           /\ UNCHANGED <<running, cleanup, pending, stopped>>

\* Apply can attach after death or another native operation removed an instance.
Lost(p) == /\ running /\ p \in active /\ active' = active \ {p}
           /\ pending' = pending \ {p}
           /\ UNCHANGED <<running, cleanup, holds, stopped>>
Apply == /\ running /\ active' = active \cup holds
         /\ UNCHANGED <<running, cleanup, holds, pending, stopped>>

\* Native removal marks the held target/definition pairs before forgetting holds.
Removed == IF cleanup THEN pending \cup (active \cap holds) ELSE pending
Clear == /\ running /\ running' = FALSE
         /\ stopped' = holds /\ holds' = {} /\ pending' = Removed
         /\ UNCHANGED <<cleanup, active>>

\* Native modifier updates drain deletion flags; no caller frees the instances.
EngineUpdate == /\ pending # {} /\ active' = active \ pending /\ pending' = {}
                /\ UNCHANGED <<running, cleanup, holds, stopped>>
WorldEnding == /\ running' = FALSE /\ active' = {} /\ pending' = {}
               /\ holds' = {} /\ stopped' = {}
               /\ UNCHANGED cleanup
Done == ~running /\ pending = {} /\ UNCHANGED vars
Next == (\E p \in Pairs : Hold(p) \/ Lost(p)) \/ Apply \/ Clear
        \/ EngineUpdate \/ WorldEnding \/ Done
Spec == Init /\ [][Next]_vars /\ WF_vars(EngineUpdate)

AdmittedCleanup == holds # {} => cleanup
StoppedMarked == ~running => stopped \cap (active \ pending) = {}
StoppedDrains == ~running ~> stopped \cap active = {}
====
