---- MODULE LayoutBinding ----
\* A mod's authored layout, loaded from its addon, with the mod's bindings
\* applied to its named panels. The renderer applies every binding the tree
\* holds once the layout loads, applies each change in the draw that brings
\* it, and applies them all again when the game builds the layout anew.
\* A press reaches the mod only from a bound panel of the current draw.
EXTENDS Naturals

VARIABLES phase, wanted, applied, press
vars == <<phase, wanted, applied, press>>

Init == /\ phase = "absent" /\ wanted = 0 /\ applied = 0 /\ press = "none"

Show == /\ phase = "absent" /\ phase' = "loading" /\ UNCHANGED <<wanted, applied, press>>
Loaded == /\ phase = "loading" /\ phase' = "bound" /\ applied' = wanted
          /\ UNCHANGED <<wanted, press>>
\* The layout's file is not in the addon, or fails to load.
Fails == /\ phase = "loading" /\ phase' = "missing" /\ UNCHANGED <<wanted, applied, press>>
Change == /\ phase # "absent" /\ wanted < 2 /\ wanted' = wanted + 1
          /\ applied' = IF phase = "bound" THEN wanted + 1 ELSE applied
          /\ UNCHANGED <<phase, press>>
Rebuilt == /\ phase = "bound" /\ phase' = "loading" /\ UNCHANGED <<wanted, applied, press>>
Press == /\ phase = "bound" /\ press' = IF applied = wanted THEN "current" ELSE "stale"
         /\ UNCHANGED <<phase, wanted, applied>>
Remove == /\ phase # "absent" /\ phase' = "absent" /\ press' = "none"
          /\ UNCHANGED <<wanted, applied>>

Next == Show \/ Loaded \/ Fails \/ Change \/ Rebuilt \/ Press \/ Remove
Spec == Init /\ [][Next]_vars

\* A loaded layout shows exactly what the mod's tree binds.
BoundShowsTheTree == phase = "bound" => applied = wanted
\* No press comes from a binding the mod has replaced.
NoStalePress == press # "stale"
====
