---- MODULE PluginLifecycle ----
\* One mod's life in WasmPlugin: the running instance, a reload, a hold that
\* keeps the instance through reloads, the successor that waits for the bots
\* its predecessor removed, a failure, and the restart a player may ask for.
\*
\* A reload loads the new build before it touches the running one, so a build
\* that does not load leaves the old one running. A build that loads replaces
\* the instance: Halt first, then a successor that Arrive starts once the bots
\* have left or the wait is overdue. A mod that fails stops alone and offers a
\* restart; a build that loads but fails to start stops the same way. Not
\* modeled: the mod's own events, worlds and the frame budget.
EXTENDS Naturals
VARIABLES
  running,    \* an instance is attached and started
  copies,     \* instances alive that have started and not halted
  held,       \* the running instance keeps itself through reloads
  waiting,    \* a build waits for the hold to end
  succ,       \* a successor waits to start
  departing,  \* a bot the old instance removed has not left
  overdue,    \* the successor's wait has passed
  offer,      \* the restart players may ask for
  gone,       \* the plugin was stopped
  builds      \* builds brought so far, bounded to keep the model finite
vars == <<running, copies, held, waiting, succ, departing, overdue, offer, gone, builds>>

Init ==
  /\ running = TRUE /\ copies = 1 /\ held = FALSE /\ waiting = FALSE
  /\ succ = FALSE /\ departing = FALSE /\ overdue = FALSE
  /\ offer = "none" /\ gone = FALSE /\ builds = 0

\* Halt: the instance, its hold and its offer's subject go.
Halted ==
  /\ running' = FALSE /\ held' = FALSE
  /\ copies' = IF running THEN copies - 1 ELSE copies

\* Succeed: halt a running instance and queue the new one.
Succeed ==
  /\ Halted /\ succ' = TRUE /\ overdue' = FALSE
  /\ \E d \in BOOLEAN : departing' = d

\* Reload brings a build: held, it waits; one that does not load changes
\* nothing; one that loads succeeds the instance.
Reload ==
  /\ ~gone /\ builds < 2 /\ builds' = builds + 1
  /\ IF held
     THEN waiting' = TRUE /\ UNCHANGED <<running, copies, held, succ, departing, overdue, offer, gone>>
     ELSE \/ UNCHANGED <<running, copies, held, waiting, succ, departing, overdue, offer, gone>>
          \/ Succeed /\ UNCHANGED <<waiting, offer, gone>>

\* Tick: the waiting build replaces the instance once the hold ends.
Waiting ==
  /\ ~gone /\ waiting /\ ~held
  /\ waiting' = FALSE
  /\ \/ UNCHANGED <<running, copies, held, succ, departing, overdue, offer, gone, builds>>
     \/ Succeed /\ UNCHANGED <<offer, gone, builds>>

\* Arrive: the successor starts when no bot is leaving or the wait is overdue.
\* A build attached with it withdraws the offer; a start that fails stops the
\* mod, which offers a restart.
Arrive ==
  /\ ~gone /\ succ /\ (~departing \/ overdue)
  /\ succ' = FALSE
  /\ \/ running' = TRUE /\ copies' = copies + 1 /\ offer' = "none"
     \/ running' = FALSE /\ copies' = copies /\ offer' = "stopped"
  /\ UNCHANGED <<held, waiting, departing, overdue, gone, builds>>

\* Environment: the bots leave, and the wait runs out.
Leave == ~gone /\ departing /\ departing' = FALSE
         /\ UNCHANGED <<running, copies, held, waiting, succ, overdue, offer, gone, builds>>
Late == ~gone /\ succ /\ ~overdue /\ overdue' = TRUE
        /\ UNCHANGED <<running, copies, held, waiting, succ, departing, offer, gone, builds>>

\* The running mod holds itself through reloads, or lets go.
Hold ==
  /\ ~gone /\ running /\ held' \in BOOLEAN
  /\ UNCHANGED <<running, copies, waiting, succ, departing, overdue, offer, gone, builds>>

\* The mod fails: it stops alone and offers a restart.
Fail ==
  /\ ~gone /\ running
  /\ Halted /\ offer' = "stopped"
  /\ UNCHANGED <<waiting, succ, departing, overdue, gone, builds>>

\* The mod runs slowly: players may restart it for a while.
Slow ==
  /\ ~gone /\ running /\ offer \in {"none", "slow"} /\ offer' = "slow"
  /\ UNCHANGED <<running, copies, held, waiting, succ, departing, overdue, gone, builds>>
SlowEnds ==
  /\ offer = "slow" /\ offer' = "none"
  /\ UNCHANGED <<running, copies, held, waiting, succ, departing, overdue, gone, builds>>

\* A player asks while a restart is offered.
Ask ==
  /\ ~gone /\ offer \in {"slow", "stopped", "asked"} /\ offer' = "asked"
  /\ UNCHANGED <<running, copies, held, waiting, succ, departing, overdue, gone, builds>>

\* Restart: a successor on its way is the restart; a build that no longer loads
\* stays stopped and offered; otherwise the instance is succeeded.
Restart ==
  /\ ~gone /\ offer = "asked"
  /\ IF succ
     THEN offer' = "none"
          /\ UNCHANGED <<running, copies, held, waiting, succ, departing, overdue, gone, builds>>
     ELSE \/ Halted /\ offer' = "stopped"
             /\ UNCHANGED <<waiting, succ, departing, overdue, gone, builds>>
          \/ Succeed /\ offer' = "none" /\ UNCHANGED <<waiting, gone, builds>>

\* Stop drops everything the plugin holds.
Stop ==
  /\ ~gone /\ gone' = TRUE
  /\ Halted /\ waiting' = FALSE /\ succ' = FALSE /\ offer' = "none"
  /\ UNCHANGED <<departing, overdue, builds>>

Progress == Waiting \/ Arrive \/ Restart \/ Late
Next == Reload \/ Waiting \/ Arrive \/ Leave \/ Late \/ Hold \/ Fail \/ Slow
        \/ SlowEnds \/ Ask \/ Restart \/ Stop \/ UNCHANGED vars
Spec == Init /\ [][Next]_vars /\ WF_vars(Progress)

\* A reload never runs two copies of a mod, nor an instance beside a successor.
OneCopy == copies <= 1 /\ copies = (IF running THEN 1 ELSE 0) /\ ~(running /\ succ)
HeldIsRunning == held => running
\* A mod that is not running and has no successor offers a restart, so a
\* failure never leaves it dead with no way back.
NoDeadEnd == ~gone /\ ~running /\ ~succ => offer \in {"stopped", "asked"}
StoppedIsEmpty == gone => ~running /\ ~succ /\ ~waiting /\ offer = "none"
\* A successor starts, and a player's request is carried out.
SuccessorStarts == succ ~> ~succ
AskedIsAnswered == offer = "asked" ~> offer # "asked"
====
