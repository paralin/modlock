---- MODULE Admission ----
\* admit.go Admission.Run, Wake, holdUntilChanged and hold. Service/Host
\* owns one admission loop per peer. It rereads sign-in on each attempt,
\* waits for a Wake after refusal, retries a dropped socket, and closes an
\* older socket when a Wake observes different sign-in. The environment
\* supplies Wake after sign-in changes; the buffered channel coalesces them.
\* Token 1 is refused, token 2 is accepted; each new token appears once.
\* Socket establishment eventually answers when connectivity recovers.
\* Server-side signature/token verification and permanent network outages
\* are outside this client's lifetime model; GameLink checks game admission.
EXTENDS Naturals
CONSTANT ObserveChange
VARIABLE s
Init == s = [token |-> 0, used |-> {}, attempt |-> 0, refused |-> 0,
             stage |-> "idle", wake |-> FALSE, active |-> FALSE]
SignIn(t) == /\ t \notin s.used
             /\ s' = [s EXCEPT !.token = t, !.used = @ \cup {t}, !.wake = TRUE]
SignOut == /\ s.token # 0 /\ s' = [s EXCEPT !.token = 0, !.wake = TRUE]
Try == /\ s.stage = "idle" /\ s.token # 0 /\ s.token # s.refused
       /\ s' = [s EXCEPT !.attempt = s.token, !.stage = "connecting"]
WaitSignIn == /\ s.stage = "idle" /\ (s.token = 0 \/ s.token = s.refused)
              /\ s' = [s EXCEPT !.stage = "waiting"]
Accepted == /\ s.stage = "connecting" /\ s.attempt = 2
            /\ s' = [s EXCEPT !.stage = "admitted", !.active = TRUE]
Refused == /\ s.stage = "connecting" /\ s.attempt = 1
           /\ s' = [s EXCEPT !.stage = "waiting", !.refused = s.attempt]
Dropped == /\ s.stage = "admitted"
           /\ s' = [s EXCEPT !.stage = "retry", !.active = FALSE]
Retry == /\ s.stage = "retry" /\ s' = [s EXCEPT !.stage = "idle"]
ConsumeWake == /\ s.wake
  /\ s' = [s EXCEPT !.wake = FALSE,
      !.active = IF ObserveChange /\ s.token # s.attempt THEN FALSE ELSE @,
      !.stage = IF s.stage \in {"idle", "waiting", "retry"} \/
                   (ObserveChange /\ s.token # s.attempt) THEN "idle" ELSE @]
Next == (\E t \in {1, 2} : SignIn(t)) \/ SignOut \/ Try \/ WaitSignIn
        \/ Accepted \/ Refused \/ Dropped \/ Retry \/ ConsumeWake \/ UNCHANGED s
Spec == Init /\ [][Next]_s
FairSpec == Spec /\ SF_s(Try) /\ WF_s(WaitSignIn) /\ WF_s(Accepted)
                /\ WF_s(Refused) /\ WF_s(Retry) /\ WF_s(ConsumeWake)
RefusedNotConnected == s.active => s.attempt # s.refused
FreshAfterWake == s.active /\ ~s.wake => s.attempt = s.token
RecoveredAdmission == (<>[](s.token = 2)) => <>(s.active /\ s.attempt = 2)
====
