---- MODULE TeamChange ----
\* PawnObserver::MovePlayer calls ChangeTeamKeepingHero::Move for controller
\* or pawn disagreement. The native controller wrapper preserves equipment
\* but skips its pawn when its own team already matches. The primitive then
\* runs the pawn's native transition with teardown disabled and restores it.
\* Seating admission must consider both teams before skipping the primitive.
\* These are synchronous engine-thread steps; no other caller interleaves.
\* Hero selection, disconnect and teamless players are outside this operation.
EXTENDS Naturals
VARIABLES controller, pawn, target, teardown, heroIntact, phase
vars == <<controller, pawn, target, teardown, heroIntact, phase>>
Init == /\ controller \in {2, 3} /\ pawn \in {2, 3} /\ target \in {2, 3}
        /\ teardown = TRUE /\ heroIntact = TRUE /\ phase = "admission"

\* Seating::SeatAssignedPlayer skips native work only when both teams agree.
SeatingMatches == controller = target /\ pawn = target
Admit == /\ phase = "admission"
         /\ phase' = IF SeatingMatches THEN "done" ELSE "controller"
         /\ UNCHANGED <<controller, pawn, target, teardown, heroIntact>>

\* Native controller ChangeTeam has an early return before the pawn callback.
ControllerMove == /\ phase = "controller"
                  /\ pawn' = IF controller = target THEN pawn ELSE target
                  /\ controller' = target /\ phase' = "guard"
                  /\ UNCHANGED <<target, teardown, heroIntact>>
Guard == /\ phase = "guard" /\ teardown' = FALSE /\ phase' = "pawn"
         /\ UNCHANGED <<controller, pawn, target, heroIntact>>
PawnMove == /\ phase = "pawn" /\ pawn' = target /\ phase' = "restore"
            /\ heroIntact' = (heroIntact /\ ~(pawn # target /\ teardown))
            /\ UNCHANGED <<controller, target, teardown>>
Restore == /\ phase = "restore" /\ teardown' = TRUE /\ phase' = "done"
           /\ UNCHANGED <<controller, pawn, target, heroIntact>>
Done == phase = "done" /\ UNCHANGED vars
Next == Admit \/ ControllerMove \/ Guard \/ PawnMove \/ Restore \/ Done
Spec == Init /\ [][Next]_vars
Converged == phase = "done" => controller = target /\ pawn = target
HeroPreserved == heroIntact
GuardRestored == phase = "done" => teardown
====
