---- MODULE VirtualList ----
\* A long list the renderer draws only in part. Rows are known by their
\* keys, so focus and selection follow a row as others arrive, leave or
\* move, and focus steps to a neighbor when its own row leaves.
EXTENDS Naturals, FiniteSets

Keys == {1, 2, 3}
VARIABLES rows, focus, selected
vars == <<rows, focus, selected>>

Init == /\ rows = {} /\ focus = 0 /\ selected = 0

\* Neighbor is the row after k, or the one before it at the end, or none.
Neighbor(k, rs) == IF \E r \in rs : r > k THEN CHOOSE r \in rs : r > k /\ \A s \in rs : s > k => r <= s
                   ELSE IF rs = {} THEN 0 ELSE CHOOSE r \in rs : \A s \in rs : s <= r

Insert(k) == /\ k \notin rows /\ rows' = rows \cup {k} /\ UNCHANGED <<focus, selected>>
Remove(k) == /\ k \in rows /\ rows' = rows \ {k}
             /\ focus' = IF focus = k THEN Neighbor(k, rows \ {k}) ELSE focus
             /\ selected' = IF selected = k THEN 0 ELSE selected
Focus(k) == /\ k \in rows /\ focus' = k /\ UNCHANGED <<rows, selected>>
Select == /\ focus # 0 /\ selected' = focus /\ UNCHANGED <<rows, focus>>
Clear == /\ rows # {} /\ rows' = {} /\ focus' = 0 /\ selected' = 0

Next == Select \/ Clear \/ \E k \in Keys : Insert(k) \/ Remove(k) \/ Focus(k)
Spec == Init /\ [][Next]_vars

\* Focus and selection name rows the list holds, or nothing.
FocusIsARow == focus \in rows \cup {0}
SelectionIsARow == selected \in rows \cup {0}
====
