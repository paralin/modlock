---- MODULE LobbyApplication ----
\* The store offers a local host an opaque configuration. A failed reload
\* returns the store to its confirmed configuration, which is still running.
EXTENDS Naturals
CONSTANT PreserveRunning
Ids == {"a", "b"}
VARIABLES source, offered, confirmed, pending, failed, run, failedRun
vars == <<source, offered, confirmed, pending, failed, run, failedRun>>
Init == source = "a" /\ offered = "a" /\ confirmed = "a" /\ pending = FALSE /\ failed = FALSE /\ run = 0 /\ failedRun = 0
Apply == \E id \in Ids:
  /\ ~pending /\ id # confirmed
  /\ source' = id /\ pending' = TRUE /\ failed' = FALSE
  /\ UNCHANGED <<offered, confirmed, run, failedRun>>
Offer == pending /\ offered # source /\ offered' = source
  /\ UNCHANGED <<source, confirmed, pending, failed, run, failedRun>>
Adopt == pending /\ offered = source
  /\ confirmed' = offered /\ run' = 1 - run /\ pending' = FALSE
  /\ UNCHANGED <<source, offered, failed, failedRun>>
Refuse == pending /\ offered = source
  /\ source' = confirmed /\ pending' = FALSE /\ failed' = TRUE /\ failedRun' = run
  /\ UNCHANGED <<offered, confirmed, run>>
\* Before the correction the daemon reloaded the successful setup when it
\* reappeared after a refusal, restarting the game the host had preserved.
RepeatConfirmed == failed /\ offered # source
  /\ offered' = source
  /\ run' = (IF PreserveRunning THEN run ELSE 1 - run)
  /\ UNCHANGED <<source, confirmed, pending, failed, failedRun>>
Reconnect == UNCHANGED vars
Next == Apply \/ Offer \/ Adopt \/ Refuse \/ RepeatConfirmed \/ Reconnect
FailedApplyKeepsGame == failed => run = failedRun
====
