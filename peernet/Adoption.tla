---- MODULE Adoption ----
\* A socket handles its first answer before replay. Receiving a result records
\* it before the synchronous write, and a failed write preserves it for the next
\* socket. Configuration payloads and transport encoding are abstracted by ids.
EXTENDS Naturals
VARIABLES phase, offered, confirmed, successful, result, received, wire, expectedWire, apply, expectedApply
vars == <<phase, offered, confirmed, successful, result, received, wire, expectedWire, apply, expectedApply>>
None == <<0, FALSE>>
Init == /\ phase = "down" /\ offered = 0 /\ confirmed = 0 /\ successful = 0
        /\ result = None /\ received = None /\ wire = None /\ expectedWire = None
        /\ apply = FALSE /\ expectedApply = FALSE
Needs(next) == next # 0 /\ next # offered /\ next # confirmed
Open(next) == /\ phase = "down"
              /\ phase' = IF result = None THEN "ready" ELSE "replay"
              /\ offered' = IF next = 0 THEN offered ELSE next
              /\ apply' = Needs(next) /\ expectedApply' = Needs(next)
              /\ UNCHANGED <<confirmed, successful, result, received, wire, expectedWire>>
Offer(next) == /\ phase = "ready"
               /\ offered' = IF next = 0 THEN offered ELSE next
               /\ apply' = Needs(next) /\ expectedApply' = Needs(next)
               /\ UNCHANGED <<phase, confirmed, successful, result, received, wire, expectedWire>>
Receive(id, ok) == /\ phase = "ready" /\ phase' = "live"
                   /\ result' = <<id, ok>> /\ received' = <<id, ok>>
                   /\ confirmed' = IF ok THEN id ELSE confirmed
                   /\ successful' = IF ok THEN id ELSE successful
                   /\ UNCHANGED <<offered, wire, expectedWire, apply, expectedApply>>
WriteLive == /\ phase = "live" /\ phase' = "ready"
             /\ wire' = result /\ expectedWire' = received
             /\ UNCHANGED <<offered, confirmed, successful, result, received, apply, expectedApply>>
Replay == /\ phase = "replay" /\ phase' = "ready"
          /\ wire' = result /\ expectedWire' = received
          /\ UNCHANGED <<offered, confirmed, successful, result, received, apply, expectedApply>>
WriteFails == /\ phase \in {"live", "replay"} /\ phase' = "down"
              /\ result' = result
              /\ UNCHANGED <<offered, confirmed, successful, received, wire, expectedWire, apply, expectedApply>>
Drop == /\ phase = "ready" /\ phase' = "down"
        /\ UNCHANGED <<offered, confirmed, successful, result, received, wire, expectedWire, apply, expectedApply>>
Next == (\E next \in 0..2 : Open(next) \/ Offer(next))
        \/ (\E id \in 1..2, ok \in BOOLEAN : Receive(id, ok))
        \/ WriteLive \/ Replay \/ WriteFails \/ Drop
Spec == Init /\ [][Next]_vars
SuccessfulIdentity == confirmed = successful
RetainedResult == result = received
ReplayIdentity == wire = expectedWire
NoRedundantApply == apply = expectedApply
====
