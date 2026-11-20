---- MODULE StockPanel ----
\* One game panel under two writers. StockGuard.tla treats the game's panels
\* as one part that only the guard writes. That holds for most panels, but the
\* game's own style sheets switch the visibility of some by the HUD's state:
\* the souls show only under a class the game adds once a match is under way,
\* and the minimap collapses in hero testing. A style change writes the
\* panel's visible, the same flag the guard writes, so the flag has two
\* writers and no value read from it stays true for long.
\*
\* Callers, from source: hidePanel and restorePanel in src/guard.ts are the
\* guard's only writes to a panel's visible. HudParts runs them for every
\* part, on each 0.1 s tick of StockGuard: hide while a live screen claims
\* the part, restore otherwise. A host that keeps its own guard runs the
\* same two functions for its own claims.
\*
\* The rule the two functions keep: the guard writes false only on a panel it
\* sees shown at that moment, marks it "shown" in the same step, and the mark
\* stands until a restore writes true. A restore never writes false. Whatever
\* the game does meanwhile, a panel the game shows is then visible whenever no
\* mark stands on it.
\*
\* The converse is not kept. A panel the guard hid while it showed, which the
\* game's style collapses while it is hidden, is shown by the restore until
\* the game's style next changes. The guard cannot read the style's wish, so
\* it must choose, and it shows: a panel shown by mistake costs a moment, and
\* a panel hidden by mistake stays hidden in every later game.
EXTENDS Naturals

Marks == {"none", "shown", "unseen"}

VARIABLES
  wish,     \* the game's style sheets show the panel in the HUD's state now
  visible,  \* the panel's visible flag, written by the style and the guard
  mark,     \* the guard's mark on the panel
  claimed   \* the guard is allowed and a live screen claims the panel

vars == <<wish, visible, mark, claimed>>

Init ==
  /\ wish \in BOOLEAN
  /\ visible = wish
  /\ mark = "none"
  /\ claimed = FALSE

\* Style is the game applying its style to the panel: on a change of the
\* HUD's state, or again with the same result. It writes the flag either way.
Style ==
  /\ \E w \in BOOLEAN: wish' = w /\ visible' = w
  /\ UNCHANGED <<mark, claimed>>

\* Claim and Unclaim are everything that turns the guard's want for this
\* panel: a screen, the lease, a leave, the hideout.
Claim == claimed' = TRUE /\ UNCHANGED <<wish, visible, mark>>
Unclaim == claimed' = FALSE /\ UNCHANGED <<wish, visible, mark>>

\* Hide is hidePanel. A panel that shows is hidden and marked shown, whatever
\* mark stood; one that does not show is only marked as met.
Hide ==
  /\ mark' = IF visible THEN "shown" ELSE IF mark = "none" THEN "unseen" ELSE mark
  /\ visible' = FALSE

\* Restore is restorePanel. It shows what the mark records as shown and
\* leaves every other panel's flag to the game.
Restore ==
  /\ visible' = IF mark = "shown" THEN TRUE ELSE visible
  /\ mark' = "none"

\* Tick is the guard's 0.1 s reconcile for this panel.
Tick ==
  /\ IF claimed THEN Hide ELSE Restore
  /\ UNCHANGED <<wish, claimed>>

Next == Style \/ Claim \/ Unclaim \/ Tick

\* Only the guard's tick always runs; the game restyles when it likes.
Spec == Init /\ [][Next]_vars /\ WF_vars(Tick)

TypeOK ==
  /\ wish \in BOOLEAN /\ visible \in BOOLEAN
  /\ mark \in Marks /\ claimed \in BOOLEAN

\* ShownWhenUnmarked: a panel the game shows is visible whenever no mark
\* stands on it, so a restore leaves nothing of the guard's behind.
ShownWhenUnmarked == mark = "none" /\ wish => visible

\* HiddenOnlyUnderShown: the flag is false against the game's wish only under
\* a "shown" mark, the one a restore turns into a write of true.
HiddenOnlyUnderShown == wish /\ ~visible => mark = "shown"

\* Hides: a claimed panel is hidden within a tick, however often the game
\* shows it again.
Hides == [](claimed => <>(~visible \/ ~claimed))

\* Returns: once nothing claims the panel, its mark goes within a tick, and
\* with it every write of the guard's.
Returns == [](~claimed => <>(mark = "none" \/ claimed))
====
