---- MODULE RangeControl ----
\* A control with two handles, low and high, on positions 0..2. The renderer
\* controls the handles while the player drags one; the mod's tree controls
\* them otherwise. drawn is what the mod last drew and reported what it last
\* heard. The mod hears values only when a drag ends or a step lands, and a
\* cancelled drag puts the handles where the mod last drew them.
EXTENDS Integers

Positions == 0..2
VARIABLES phase, grabbed, lo, hi, drawn, reported
vars == <<phase, grabbed, lo, hi, drawn, reported>>

Init == /\ phase = "absent" /\ grabbed = "none"
        /\ lo = 0 /\ hi = 2 /\ drawn = <<0, 2>> /\ reported = <<0, 2>>

\* The mod draws the control with its values, or redraws it with new ones
\* while no drag runs.
Show == /\ phase = "absent" /\ phase' = "idle"
        /\ lo' = drawn[1] /\ hi' = drawn[2]
        /\ UNCHANGED <<grabbed, drawn, reported>>
ModSets(l, h) == /\ phase = "idle" /\ l <= h
                 /\ lo' = l /\ hi' = h /\ drawn' = <<l, h>>
                 /\ UNCHANGED <<phase, grabbed, reported>>

\* A keyboard or controller step moves the focused handle one position and
\* reports at once, keeping the handles in order.
Step(h, d) == /\ phase = "idle"
              /\ LET l == IF h = "lo" THEN lo + d ELSE lo
                     u == IF h = "hi" THEN hi + d ELSE hi
                 IN /\ l \in Positions /\ u \in Positions /\ l <= u
                    /\ lo' = l /\ hi' = u /\ reported' = <<l, u>>
              /\ UNCHANGED <<phase, grabbed, drawn>>

\* Grabbing a handle captures the pointer, even outside the control's box.
Grab(h) == /\ phase = "idle" /\ phase' = "dragging" /\ grabbed' = h
           /\ UNCHANGED <<lo, hi, drawn, reported>>
Move(p) == /\ phase = "dragging"
           /\ IF grabbed = "lo" THEN /\ p <= hi /\ lo' = p /\ UNCHANGED hi
                                ELSE /\ p >= lo /\ hi' = p /\ UNCHANGED lo
           /\ UNCHANGED <<phase, grabbed, drawn, reported>>
\* A mod's redraw during a drag leaves the dragged handles alone; the
\* control keeps the mod's values for a cancel.
ModSetsWhileDragging(l, h) == /\ phase = "dragging" /\ l <= h
                              /\ drawn' = <<l, h>>
                              /\ UNCHANGED <<phase, grabbed, lo, hi, reported>>
Release == /\ phase = "dragging" /\ phase' = "idle" /\ grabbed' = "none"
           /\ reported' = <<lo, hi>> /\ UNCHANGED <<lo, hi, drawn>>
\* A stock overlay, the menu's closing or the control's disabling cancels the
\* drag.
Cancel == /\ phase = "dragging" /\ phase' = "idle" /\ grabbed' = "none"
          /\ lo' = drawn[1] /\ hi' = drawn[2] /\ UNCHANGED <<drawn, reported>>
\* The control goes, mid-drag or not, and the mod hears nothing of a drag.
Lose == /\ phase # "absent" /\ phase' = "absent" /\ grabbed' = "none"
        /\ UNCHANGED <<lo, hi, drawn, reported>>

Next == Show \/ Release \/ Cancel \/ Lose
        \/ \E a, b \in Positions : ModSets(a, b) \/ ModSetsWhileDragging(a, b)
        \/ \E k \in {"lo", "hi"}, d \in {-1, 1} : Step(k, d)
        \/ \E g \in {"lo", "hi"} : Grab(g)
        \/ \E q \in Positions : Move(q)
Spec == Init /\ [][Next]_vars

HandlesKeepOrder == lo <= hi
GrabbedOnlyWhileDragging == (phase = "dragging") = (grabbed # "none")
\* Outside a drag the player sees what the mod drew or last heard, never a
\* value the mod has not seen.
ShownIsReported == phase = "idle" => <<lo, hi>> \in {drawn, reported}
====
