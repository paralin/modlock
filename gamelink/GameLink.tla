---- MODULE GameLink ----
\* gamelink Dial/requestAdmission, ReadGeneration/AcceptGeneration, Forward;
\* peernet/host.go hosting.HandleMountedStream and Service.Join. A transport
\* must authenticate the expected host; the host compares the guest's session
\* capability to the running generation before forwarding. AcceptGeneration
\* follows the UDP socket reservation; the guest binds only after acceptance.
\* Callers: peernet hosted game and Hyperline's connector host forwarding.
\* The generation is a bearer capability, not per-player authorization.
\* Rendezvous admission and eventual transport reachability are outside this
\* handshake model (Relay.tla covers route availability). A deadline/cancel
\* closes an unfinished handshake. Revocation of already accepted streams is
\* through the host context, not a recheck on every game packet.
EXTENDS Naturals
CONSTANTS CheckGeneration, AcceptAfterSocket
VARIABLE s
Init == s = [stage |-> "dial", correct |-> FALSE, socket |-> FALSE,
             accepted |-> FALSE, packets |-> FALSE, stopped |-> FALSE]
Dial(correct) == /\ s.stage = "dial"
                 /\ s' = [s EXCEPT !.stage = "request", !.correct = correct]
Request == /\ s.stage = "request"
           /\ s' = [s EXCEPT !.stage = IF s.correct \/ ~CheckGeneration THEN "reserve" ELSE "closed"]
Reserve == /\ s.stage = "reserve"
           /\ s' = [s EXCEPT !.socket = TRUE, !.stage = "accept"]
Accept == /\ s.stage = "accept" \/ (s.stage = "reserve" /\ ~AcceptAfterSocket)
          /\ s' = [s EXCEPT !.accepted = TRUE, !.stage = "ready"]
Packets == /\ s.stage = "ready" /\ s' = [s EXCEPT !.packets = TRUE]
Stop == /\ s.stage # "closed"
        /\ s' = [s EXCEPT !.stage = "closed", !.stopped = TRUE, !.packets = FALSE]
Next == (\E correct \in BOOLEAN : Dial(correct)) \/ Request \/ Reserve \/ Accept \/ Packets \/ Stop \/ UNCHANGED s
Spec == Init /\ [][Next]_s
CapabilityRequired == s.accepted => s.correct
SocketBeforeAnswer == s.accepted => s.socket
StoppedHasNoPackets == s.stopped => ~s.packets
====
