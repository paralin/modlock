---- MODULE Relay ----
\* relay.go Relay.Hold/ICEConfig/relayServers; lobby.go holdLobby and
\* holdLobbySocket. Host/Join use these ICE servers for new links. A relay
\* is offered only with TURN URLs and credentials valid past the one-minute
\* margin. Loss of signaling keeps its last credentials until they expire;
\* closing the hosted lobby drops them. Admission to signaling authenticates
\* the peer; game admission is the session capability in GameLink.tla.
\* Network eventual reachability is conditional: at least one working path
\* must exist. Permanent network failure and expired TURN credentials cannot
\* promise a connection. Two link attempts suffice to check replacement.
EXTENDS Naturals
CONSTANT CheckExpiry
VARIABLE s
Init == s = [lobby |-> "open", relay |-> "none", direct |-> FALSE,
             link |-> "new", offeredExpired |-> FALSE]
Credentials == /\ s.lobby = "open" /\ s.relay # "valid"
               /\ s' = [s EXCEPT !.relay = "valid"]
Expire == /\ s.relay = "valid" /\ s' = [s EXCEPT !.relay = "expired"]
DirectAvailable == /\ ~s.direct /\ s' = [s EXCEPT !.direct = TRUE]
Close == /\ s.lobby = "open"
         /\ s' = [s EXCEPT !.lobby = "closed", !.relay = "none"]
Offer == /\ s.link = "new"
         /\ s' = [s EXCEPT !.offeredExpired = s.relay = "expired" /\ ~CheckExpiry,
              !.link = IF s.direct \/ s.relay = "valid" THEN "connected" ELSE "waiting"]
Retry == /\ s.link = "waiting" /\ (s.direct \/ s.relay = "valid")
         /\ s' = [s EXCEPT !.link = "new"]
Next == Credentials \/ Expire \/ DirectAvailable \/ Close \/ Offer \/ Retry \/ UNCHANGED s
Spec == Init /\ [][Next]_s
FairSpec == Spec /\ WF_s(Offer) /\ WF_s(Retry)
FreshCredentials == ~s.offeredExpired
\* Direct reachability stays available once learned in this bounded model.
ReachableConnects == s.direct ~> s.link = "connected"
RelayConnects == (<>[](s.relay = "valid")) => <>(s.link = "connected")
====
