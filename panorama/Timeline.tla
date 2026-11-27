---- MODULE Timeline ----
\* One timed presentation: its steps of style and sound run on one clock,
\* the renderer's wall clock or the game's clock, which stops while the game
\* pauses. A restart runs it again from its start, and a cancel, which drops
\* it from the root, takes back its looks with nothing left to play.
EXTENDS Naturals

Clocks == {"wall", "game"}
VARIABLES phase, clock, t, paused
vars == <<phase, clock, t, paused>>

Init == /\ phase = "idle" /\ clock = "wall" /\ t = 0 /\ paused = FALSE

Start(c) == /\ phase \in {"idle", "done"} /\ clock' = c /\ t' = 0
            /\ phase' = IF c = "game" /\ paused THEN "held" ELSE "running"
            /\ UNCHANGED paused
Restart == /\ phase \in {"running", "held"} /\ t' = 0 /\ UNCHANGED <<phase, clock, paused>>
Cancel == /\ phase \in {"running", "held", "done"} /\ phase' = "idle" /\ t' = 0
          /\ UNCHANGED <<clock, paused>>
Tick == /\ phase = "running"
        /\ IF t < 2 THEN t' = t + 1 /\ UNCHANGED phase ELSE phase' = "done" /\ UNCHANGED t
        /\ UNCHANGED <<clock, paused>>
Pause == /\ ~paused /\ paused' = TRUE
         /\ phase' = IF phase = "running" /\ clock = "game" THEN "held" ELSE phase
         /\ UNCHANGED <<clock, t>>
Resume == /\ paused /\ paused' = FALSE
          /\ phase' = IF phase = "held" THEN "running" ELSE phase
          /\ UNCHANGED <<clock, t>>

Next == Restart \/ Cancel \/ Tick \/ Pause \/ Resume \/ \E c \in Clocks : Start(c)
Spec == Init /\ [][Next]_vars

\* A game-clock presentation stands still while the game pauses.
GameClockHoldsInPause == (phase = "running" /\ clock = "game") => ~paused
\* Only a game-clock presentation is ever held.
OnlyGameClockHolds == phase = "held" => clock = "game" /\ paused
====
