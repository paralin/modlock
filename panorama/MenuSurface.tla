---- MODULE MenuSurface ----
EXTENDS Naturals

VARIABLES phase, mode, held, request
vars == <<phase, mode, held, request>>
Modes == {"manual", "toggle", "hold"}
Init == /\ phase = "absent" /\ mode = "manual" /\ held = FALSE /\ request = 0

Mount == /\ phase = "absent" /\ phase' = "closed"
         /\ mode' \in Modes /\ UNCHANGED <<held, request>>
Press == /\ held = FALSE /\ held' = TRUE
         /\ phase' = CASE phase = "closed" /\ mode # "manual" -> "open"
                       [] phase = "open" /\ mode = "toggle" -> "closed"
                       [] OTHER -> phase
         /\ UNCHANGED <<mode, request>>
Release == /\ held = TRUE /\ held' = FALSE
           /\ phase' = IF phase = "open" /\ mode = "hold" THEN "closed" ELSE phase
           /\ UNCHANGED <<mode, request>>
Dismiss == /\ phase = "open" /\ phase' = "closed"
           /\ UNCHANGED <<mode, held, request>>
StockTakes == /\ phase \in {"open", "closed"} /\ phase' = "stock"
              /\ UNCHANGED <<mode, held, request>>
StockReturns == /\ phase = "stock" /\ phase' = "closed"
                /\ UNCHANGED <<mode, held, request>>
Lose == /\ phase # "absent" /\ phase' = "absent"
        /\ UNCHANGED <<mode, held, request>>
OpenRequest == /\ phase # "absent" /\ request < 2 /\ request' = request + 1
               /\ phase' = IF phase = "stock" THEN phase ELSE "open"
               /\ UNCHANGED <<mode, held>>
Replay == UNCHANGED vars
Next == Mount \/ Press \/ Release \/ Dismiss \/ StockTakes \/ StockReturns \/ Lose \/ OpenRequest \/ Replay
Spec == Init /\ [][Next]_vars

OwnsCapture == phase = "open"
ReleaseReturnsControls == [][(Release /\ mode = "hold") => ~OwnsCapture']_vars
DismissReturnsControls == [][Dismiss => ~OwnsCapture']_vars
SurfaceLossReturnsControls == [][(Lose \/ StockTakes) => ~OwnsCapture']_vars
====
