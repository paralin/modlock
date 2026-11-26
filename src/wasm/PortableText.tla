---- MODULE PortableText ----
EXTENDS Naturals

(* A fragment can advance only the one submission currently bound to a form.
   Two units stand for any bounded byte count; UTF-8 is checked at finish. *)
VARIABLES control, phase, serial, total, bytes, accepted
vars == <<control, phase, serial, total, bytes, accepted>>
Init == /\ control = 1 /\ phase = "idle" /\ serial = 0
        /\ total = 0 /\ bytes = 0 /\ accepted = 0
Begin == /\ phase = "idle" /\ serial < 2 /\ serial' = serial + 1
         /\ phase' = "sending" /\ total' \in 0..2 /\ bytes' = 0
         /\ UNCHANGED <<control, accepted>>
Part == /\ phase = "sending" /\ bytes < total /\ bytes' = bytes + 1
        /\ UNCHANGED <<control, phase, serial, total, accepted>>
Finish == /\ phase = "sending" /\ bytes = total /\ phase' = "idle"
          /\ accepted' = accepted + 1 /\ UNCHANGED <<control, serial, total, bytes>>
Refuse == /\ phase = "sending" /\ phase' = "idle"
          /\ UNCHANGED <<control, serial, total, bytes, accepted>>
Replace == /\ control = 1 /\ control' = 2 /\ phase' = "idle"
           /\ UNCHANGED <<serial, total, bytes, accepted>>
Replay == UNCHANGED vars
Next == Begin \/ Part \/ Finish \/ Refuse \/ Replace \/ Replay
Spec == Init /\ [][Next]_vars

AtMostOnce == accepted <= serial
DeliverOnlyWhole == [][(accepted' > accepted) => Finish]_vars
====
