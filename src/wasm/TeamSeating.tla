---- MODULE TeamSeating ----
\* Host seats are confirmed for Steam identity, slot and generation only
\* after native join finishes. A confirmed player remains under mod rules.
EXTENDS Naturals
VARIABLES target, epoch, connected, ready, serves, done, applied, nativeTeam, phase, nativeRequest
vars == <<target, epoch, connected, ready, serves, done, applied, nativeTeam, phase, nativeRequest>>
\* Four connections include reconnecting in another slot with the same generation.
Slot == epoch % 2
Generation == (epoch \div 2) + 1
Key == <<Slot, Generation, target>>
None == <<2, 0, 0>>
Pending == target # 0 /\ connected /\ done # Key
Init == /\ target = 0 /\ epoch = 0 /\ connected = FALSE
        /\ ready = FALSE /\ serves = TRUE /\ done = None
        /\ applied = None /\ nativeTeam = 0 /\ phase = "none" /\ nativeRequest = FALSE
Assign(t) == /\ t \in 0..3 /\ target' = t
             /\ done' = IF t = target THEN done ELSE None
             /\ phase' = IF t = target THEN phase ELSE "none"
             /\ UNCHANGED <<epoch, connected, ready, serves, applied, nativeTeam, nativeRequest>>
Connect == /\ ~connected /\ connected' = TRUE
           /\ UNCHANGED <<target, epoch, ready, serves, done, applied, nativeTeam, phase, nativeRequest>>
ControllerReady == /\ connected /\ ready' = TRUE
                   /\ UNCHANGED <<target, epoch, connected, serves, done, applied, nativeTeam, phase, nativeRequest>>
Disconnect == /\ connected /\ epoch < 3 /\ connected' = FALSE /\ ready' = FALSE
              /\ epoch' = epoch + 1 /\ phase' = "none"
              /\ UNCHANGED <<target, serves, done, applied, nativeTeam, nativeRequest>>
Scope(s) == /\ s \in BOOLEAN /\ serves' = s
            /\ UNCHANGED <<target, epoch, connected, ready, done, applied, nativeTeam, phase, nativeRequest>>
BeginSeat == /\ Pending /\ ready /\ serves /\ phase = "none"
             /\ nativeTeam' = target /\ phase' = "team" /\ nativeRequest' = TRUE
             /\ UNCHANGED <<target, epoch, connected, ready, serves, done, applied>>
PublishPawn == /\ Pending /\ ready /\ serves /\ nativeRequest /\ phase = "team" /\ phase' = "pawn"
               /\ UNCHANGED <<target, epoch, connected, ready, serves, done, applied, nativeTeam, nativeRequest>>
NotifyReady == /\ Pending /\ ready /\ serves /\ phase = "pawn"
               /\ phase' = "ready" /\ applied' = Key
               /\ UNCHANGED <<target, epoch, connected, ready, serves, done, nativeTeam, nativeRequest>>
\* An existing selected hero needs only a successful native team move.
ExistingHero == /\ Pending /\ ready /\ serves /\ phase = "none"
                /\ nativeTeam' = target /\ phase' = "ready" /\ applied' = Key
                /\ UNCHANGED <<target, epoch, connected, ready, serves, done, nativeRequest>>
Confirm == /\ Pending /\ ready /\ serves /\ phase = "ready" /\ nativeTeam = target /\ done' = Key
           /\ UNCHANGED <<target, epoch, connected, ready, serves, applied, nativeTeam, phase, nativeRequest>>
ModMove(t) == /\ ~Pending /\ t \in 1..3 /\ nativeTeam' = t
              /\ UNCHANGED <<target, epoch, connected, ready, serves, done, applied, phase, nativeRequest>>
Retry == UNCHANGED vars
Reload == UNCHANGED vars
WorldEnding == /\ done' = None /\ ready' = FALSE /\ nativeTeam' = 0 /\ phase' = "none"
               /\ nativeRequest' = FALSE
               /\ UNCHANGED <<target, epoch, connected, serves, applied>>
Next == (\E t \in 0..3 : Assign(t)) \/ Connect \/ ControllerReady \/ Disconnect
        \/ (\E s \in BOOLEAN : Scope(s)) \/ BeginSeat \/ PublishPawn \/ NotifyReady
        \/ ExistingHero \/ Confirm \/ (\E t \in 1..3 : ModMove(t))
        \/ Retry \/ Reload \/ WorldEnding
Spec == Init /\ [][Next]_vars
ConfirmedOnlyAfterNativeSuccess == done = Key => applied = Key
PendingRequiresCurrentConnection == Pending => connected /\ target \in 1..3
WaitingForCurrentWorld == phase = "team" /\ ready => nativeRequest
====
