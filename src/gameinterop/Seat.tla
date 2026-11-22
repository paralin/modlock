---- MODULE Seat ----
\* One player slot's seat: the controller and hero the server keeps for a
\* player from their join until they leave for good. The engine holds a
\* human's row after a disconnect, and gives the row, its controller and its
\* hero back only to that player. So a disconnect makes the seat away, not
\* gone. The seat ends only when Modlock releases the row: a kick, the lobby
\* dropping the player, or the away window running out. Only then does a mod
\* hear the leave and the seat's hero go down. A mod that lays down a held
\* hero, or a leave fired at the disconnect, hands the returning player a dead
\* seat, and the client drops the connection.
EXTENDS Naturals
CONSTANT MaxJoins
Players == {"a", "b"}
VARIABLES row, owner, seat, heard, hero, listed, joins, leaves
vars == <<row, owner, seat, heard, hero, listed, joins, leaves>>

\* row is the engine's client row: free, live (connected) or held (for owner).
\* seat is Modlock's: none, joining, joined or away. heard is what one mod was
\* told: none, joined or away. hero is the seat's hero, and listed is whether
\* its controller is on a team.
Init == /\ row = "free" /\ owner = "none" /\ seat = "none" /\ heard = "none"
        /\ hero = "none" /\ listed = FALSE /\ joins = 0 /\ leaves = 0

\* Connect is a new player taking a free row: a new seat and controller.
Connect(p) == /\ row = "free" /\ joins < MaxJoins
              /\ row' = "live" /\ owner' = p /\ seat' = "joining" /\ hero' = "none"
              /\ listed' = TRUE
              /\ UNCHANGED <<heard, joins, leaves>>

\* Return is the owner reconnecting into their held row: the same seat.
Return(p) == /\ row = "held" /\ p = owner
             /\ row' = "live" /\ seat' = "joining"
             /\ UNCHANGED <<owner, heard, hero, listed, joins, leaves>>

\* Ready is the client finishing its sign-on. A mod hears a join once per
\* seat; a returning player was never left, so they come back from away.
Ready == /\ seat = "joining" /\ seat' = "joined"
         /\ heard' = "joined"
         /\ joins' = IF heard = "none" THEN joins + 1 ELSE joins
         /\ UNCHANGED <<row, owner, hero, listed, leaves>>

\* Spawn is the mod giving the seat its hero.
Spawn == /\ seat = "joined" /\ hero = "none" /\ hero' = "alive"
         /\ UNCHANGED <<row, owner, seat, heard, listed, joins, leaves>>

\* Disconnect is the client going away. The engine holds the row.
Disconnect == /\ row = "live" /\ row' = "held" /\ seat' = "away"
              /\ heard' = IF heard = "joined" THEN "away" ELSE heard
              /\ UNCHANGED <<owner, hero, listed, joins, leaves>>

\* Leave is the seat ending: the mod hears it once and the hero goes down.
Leave == /\ seat' = "none" /\ owner' = "none" /\ row' = "free" /\ heard' = "none"
         /\ leaves' = IF heard = "none" THEN leaves ELSE leaves + 1
         /\ hero' = (IF hero = "alive" THEN "laid" ELSE hero) /\ listed' = FALSE
         /\ UNCHANGED joins

\* Kick is Modlock's DisconnectClient on a live row: release, then disconnect.
Kick == row = "live" /\ Leave

\* Release is Modlock's DisconnectClient on a held row, or the away window.
Release == row = "held" /\ Leave

\* Sweep is Strays laying down a hero that no seat holds.
Sweep == /\ hero = "alive" /\ seat = "none" /\ hero' = "laid" /\ listed' = FALSE
         /\ UNCHANGED <<row, owner, seat, heard, joins, leaves>>

Next == (\E p \in Players : Connect(p) \/ Return(p)) \/ Ready \/ Spawn
        \/ Disconnect \/ Kick \/ Release \/ Sweep
Spec == Init /\ [][Next]_vars

\* HeldSeatIntact: a row the engine holds for its owner keeps a standing hero
\* and a listed controller, so the returning player gets a live seat.
HeldSeatIntact == row = "held" => hero # "laid" /\ listed
\* OneLeavePerJoin: a mod hears each join's leave once, and only after it.
OneLeavePerJoin == /\ leaves <= joins /\ joins <= leaves + 1
                   /\ (heard = "none" => joins = leaves)
\* AwayIsHeld: a seat is away exactly while the engine holds its row.
AwayIsHeld == (seat = "away") = (row = "held")
====
