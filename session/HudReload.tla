---- MODULE HudReload ----
\* WasmPlugin.Halt clears UI; onFrame/show can replace it before the peer
\* samples Screens. The tree generation survives the sampled delta and mirror,
\* so Drawing replaces the old panels even when the nodes and URL match.
\* Ordinary node updates are omitted: they retain this generation.
EXTENDS Naturals
VARIABLES phase, hostHud, hostGeneration, guestHud, renderedGeneration, caughtUp
vars == <<phase, hostHud, hostGeneration, guestHud, renderedGeneration, caughtUp>>
Init == /\ phase = "initial" /\ hostHud = TRUE /\ hostGeneration = 1
        /\ guestHud = TRUE /\ renderedGeneration = 1 /\ caughtUp = TRUE
Halt == /\ phase = "initial" /\ phase' = "cleared"
        /\ hostHud' = FALSE /\ hostGeneration' = hostGeneration + 1
        /\ caughtUp' = FALSE /\ UNCHANGED <<guestHud, renderedGeneration>>
Show == /\ phase = "cleared" /\ phase' = "restored"
        /\ hostHud' = TRUE /\ caughtUp' = FALSE
        /\ UNCHANGED <<hostGeneration, guestHud, renderedGeneration>>
Sample == /\ guestHud' = hostHud /\ caughtUp' = TRUE
          /\ renderedGeneration' =
             IF hostHud THEN hostGeneration ELSE renderedGeneration
          /\ UNCHANGED <<phase, hostHud, hostGeneration>>
Next == Halt \/ Show \/ Sample \/ UNCHANGED vars
Spec == Init /\ [][Next]_vars
ReloadRecreatesLayout ==
  ~caughtUp \/ ~hostHud \/ renderedGeneration = hostGeneration
====
