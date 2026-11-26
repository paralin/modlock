---- MODULE MenuQueue ----
\* The renderer's one pending slot for menus whose open request arrived while
\* something blocked them. A stock overlay blocks every menu; an open menu
\* blocks another mod's request and its own mod's lower-priority requests.
\* The slot keeps one request: within a mod the higher priority, otherwise
\* the newer. It opens when nothing blocks it, unless the mod withdrew it.
EXTENDS Naturals

\* asked is history: the request the player is owed until it opens, is
\* withdrawn, or loses the slot to another request.
VARIABLES active, pending, stock, mounted, asked
vars == <<active, pending, stock, mounted, asked>>

None == "none"
Surfaces == {"warn", "alert", "other"}
Mod == [warn |-> "a", alert |-> "a", other |-> "b"]
Priority == [warn |-> 1, alert |-> 2, other |-> 1]

\* Outranks(x, s): an open menu x keeps a request for s waiting.
Outranks(x, s) == x # s /\ (Mod[x] # Mod[s] \/ Priority[x] > Priority[s])
Blocked(s) == stock \/ (active # None /\ Outranks(active, s))
\* Keep(s, p) is the request the slot keeps when s arrives and holds p.
Keep(s, p) == IF p = None \/ p = s THEN s
              ELSE IF Mod[p] = Mod[s] /\ Priority[p] > Priority[s] THEN p
              ELSE s

Init == /\ active = None /\ pending = None /\ stock = FALSE
        /\ mounted = Surfaces /\ asked = None

Request(s) == /\ s \in mounted
              /\ LET p == Keep(s, pending) IN
                   IF Blocked(p) THEN pending' = p /\ UNCHANGED active
                   ELSE active' = p /\ pending' = None
              /\ asked' = Keep(s, pending)
              /\ UNCHANGED <<stock, mounted>>
\* Tick runs after a dismissal or a stock overlay clears.
Tick == /\ pending # None /\ ~Blocked(pending)
        /\ active' = pending /\ pending' = None
        /\ asked' = None
        /\ UNCHANGED <<stock, mounted>>
Dismiss == /\ active # None /\ active' = None
           /\ asked' = IF asked = active THEN None ELSE asked
           /\ UNCHANGED <<pending, stock, mounted>>
StockTakes == /\ ~stock /\ stock' = TRUE /\ active' = None
              /\ asked' = IF asked = active THEN None ELSE asked
              /\ UNCHANGED <<pending, mounted>>
StockReturns == /\ stock /\ stock' = FALSE
                /\ UNCHANGED <<active, pending, mounted, asked>>
Withdraw(s) == /\ s \in mounted /\ mounted' = mounted \ {s}
               /\ active' = IF active = s THEN None ELSE active
               /\ pending' = IF pending = s THEN None ELSE pending
               /\ asked' = IF asked = s THEN None ELSE asked
               /\ UNCHANGED stock
Remount(s) == /\ s \notin mounted /\ mounted' = mounted \cup {s}
              /\ UNCHANGED <<active, pending, stock, asked>>

Next == \/ \E s \in Surfaces : Request(s) \/ Withdraw(s) \/ Remount(s)
        \/ Tick \/ Dismiss \/ StockTakes \/ StockReturns
\* The player eventually closes a stock overlay.
Spec == Init /\ [][Next]_vars /\ SF_vars(Tick) /\ WF_vars(StockReturns)

TypeOK == /\ active \in Surfaces \cup {None} /\ pending \in Surfaces \cup {None}
          /\ stock \in BOOLEAN /\ mounted \subseteq Surfaces
StockWins == stock => active = None
OnlyMounted == /\ (active # None => active \in mounted)
               /\ (pending # None => pending \in mounted)
\* A waiting request waits only behind a menu that outranks it.
WaitsOnlyWhenOutranked == (active # None /\ pending # None) => Outranks(active, pending)
\* A request the player is owed is open or waiting, never lost.
KeptWhenBlocked == asked # None => asked \in {active, pending}
\* A menu never gives way to a lower-priority request of its own mod.
PriorityHolds == [][(active # None /\ active' \notin {None, active}
                     /\ Mod[active'] = Mod[active])
                    => Priority[active'] >= Priority[active]]_vars
\* A dismissal never queues the menu the player closed.
DismissalStays == [][Dismiss => pending' # active]_vars
\* Once nothing blocks it and the mod keeps it, the waiting request opens.
ShowsWhenClear == [](pending # None /\ ~stock /\ active = None ~> pending = None)
====
