---- MODULE Retention ----
\* Serial draws build candidates before retaining them. A refused delta gets
\* one reset attempt; a refused reset forgets the local screen. Host entries
\* cannot interleave with a draw. Node encoding is abstracted as a constant:
\* callbacks may change while that encoding stays unchanged.
EXTENDS Naturals
VARIABLES phase, retained, callbacks, candidate, accepted, generation, tenure
vars == <<phase, retained, callbacks, candidate, accepted, generation, tenure>>
Init == /\ phase = "idle" /\ retained = 0 /\ callbacks = 0
        /\ candidate = 0 /\ accepted = 0 /\ generation = 1 /\ tenure = 0
Draw == /\ phase = "idle" /\ candidate < 2
        /\ candidate' = candidate + 1 /\ phase' = "delta"
        /\ UNCHANGED <<retained, callbacks, accepted, generation, tenure>>
Accept == /\ phase \in {"delta", "reset"}
          /\ retained' = candidate /\ callbacks' = candidate
          /\ accepted' = candidate /\ tenure' = generation /\ phase' = "idle"
          /\ UNCHANGED <<candidate, generation>>
RefuseDelta == /\ phase = "delta" /\ phase' = "reset"
               /\ UNCHANGED <<retained, callbacks, candidate, accepted, generation, tenure>>
RefuseReset == /\ phase = "reset" /\ phase' = "idle"
               /\ retained' = 0 /\ callbacks' = 0 /\ tenure' = 0
               /\ UNCHANGED <<candidate, accepted, generation>>
Return == /\ phase = "idle" /\ retained # 0
          /\ retained' = 0 /\ callbacks' = 0 /\ tenure' = 0
          /\ UNCHANGED <<phase, candidate, accepted, generation>>
Replace == /\ phase = "idle" /\ generation = 1 /\ generation' = 2
           /\ retained' = 0 /\ callbacks' = 0 /\ tenure' = 0
           /\ UNCHANGED <<phase, candidate, accepted>>
Next == Draw \/ Accept \/ RefuseDelta \/ RefuseReset \/ Return \/ Replace
Spec == Init /\ [][Next]_vars
AcceptedOnly == retained = 0 \/ retained = accepted
LatestCallbacks == callbacks = retained
CurrentConnection == retained = 0 \/ tenure = generation
====
