---- MODULE HudReload ----
\* Separate the mod's actual incarnation, the owner's wire generation, the
\* sampled peer frame and the renderer's retained layout. Clearing and showing
\* can coalesce; replacing the owner can reuse the same nodes. A mod also
\* tries to send generation 1, which the raw native boundary must discard.
\* Random identities are abstracted as distinct 1 and 3 for two owners.
EXTENDS Naturals
CONSTANTS PreserveReset, FreshRestart, SanitizeMod
VARIABLES phase, incarnation, hostHud, ownerGeneration, frameGeneration,
          frameIncarnation, frameHud, pending, guestGeneration, guestHud,
          renderedIncarnation, caughtUp, restarted
vars == <<phase, incarnation, hostHud, ownerGeneration, frameGeneration,
          frameIncarnation, frameHud, pending, guestGeneration, guestHud,
          renderedIncarnation, caughtUp, restarted>>
Init == /\ phase = "initial" /\ incarnation = 1 /\ hostHud = TRUE
        /\ ownerGeneration = 1 /\ frameGeneration = 1
        /\ frameIncarnation = 1 /\ frameHud = TRUE /\ pending = FALSE
        /\ guestGeneration = 1 /\ guestHud = TRUE
        /\ renderedIncarnation = 1 /\ caughtUp = TRUE /\ restarted = FALSE
Halt == /\ phase = "initial" /\ phase' = "cleared"
        /\ incarnation' = incarnation + 1 /\ hostHud' = FALSE
        /\ ownerGeneration' = IF PreserveReset THEN ownerGeneration + 1 ELSE ownerGeneration
        /\ caughtUp' = FALSE
        /\ UNCHANGED <<frameGeneration, frameIncarnation, frameHud, pending,
                       guestGeneration, guestHud, renderedIncarnation, restarted>>
Show == /\ phase = "cleared" /\ phase' = "restored" /\ hostHud' = TRUE
        /\ ownerGeneration' = IF SanitizeMod THEN ownerGeneration ELSE 1
        /\ caughtUp' = FALSE
        /\ UNCHANGED <<incarnation, frameGeneration, frameIncarnation, frameHud,
                       pending, guestGeneration, guestHud, renderedIncarnation, restarted>>
Restart == /\ ~restarted /\ phase # "cleared"
           /\ restarted' = TRUE /\ incarnation' = incarnation + 1
           /\ ownerGeneration' = IF FreshRestart THEN 3 ELSE 1
           /\ caughtUp' = FALSE
           /\ UNCHANGED <<phase, hostHud, frameGeneration, frameIncarnation, frameHud,
                          pending, guestGeneration, guestHud, renderedIncarnation>>
Sample == /\ frameGeneration' = ownerGeneration /\ frameIncarnation' = incarnation
          /\ frameHud' = hostHud /\ pending' = TRUE
          /\ UNCHANGED <<phase, incarnation, hostHud, ownerGeneration,
                         guestGeneration, guestHud, renderedIncarnation, caughtUp, restarted>>
Draw == /\ pending /\ pending' = FALSE /\ guestHud' = frameHud
        /\ guestGeneration' = frameGeneration
        /\ renderedIncarnation' =
           IF frameHud /\ (~guestHud \/ guestGeneration # frameGeneration)
           THEN frameIncarnation ELSE renderedIncarnation
        /\ caughtUp' = (frameIncarnation = incarnation /\ frameHud = hostHud)
        /\ UNCHANGED <<phase, incarnation, hostHud, ownerGeneration,
                       frameGeneration, frameIncarnation, frameHud, restarted>>
Next == Halt \/ Show \/ Restart \/ Sample \/ Draw \/ UNCHANGED vars
Spec == Init /\ [][Next]_vars
ReloadRecreatesLayout == ~caughtUp \/ ~hostHud \/ renderedIncarnation = incarnation
====
