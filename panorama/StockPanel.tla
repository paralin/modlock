---- MODULE StockPanel ----
\* One game panel under two writers. StockGuard.tla treats the game's panels
\* as one part that only the guard writes; this spec follows one panel's
\* visibility as Panorama computes it, with the game and the guard both
\* writing.
\*
\* Panorama, measured in the game: a panel's drawn visibility is its inline
\* style.visibility when one is set, and otherwise what its style sheets
\* give under the classes the HUD's state puts on its ancestors. Script's
\* panel.visible reads that drawn value, and writing it writes the inline
\* value: false writes "collapse", true writes "visible". Writing null clears
\* the inline value. Where the sheets name a visibility for the panel under
\* its classes, they decide again in the same frame; where they name none,
\* as for the top bar's TeamsContainer, the panel keeps the value it last
\* drew, so a panel collapsed inline and then cleared stays collapsed in
\* every later game. A visible write just before the null makes that kept
\* value the default, visible, and changes nothing where the sheets decide.
\* So a visible = true write keeps a panel drawn after the game's class for
\* it is gone, and the sheets' wish is never written into the flag; the
\* game's own scripts may still write the inline value.
\*
\* Callers, from source: hidePanel and restorePanel in src/guard.ts are the
\* guard's only writes to a panel's visibility. HudParts runs them for every
\* part, on each 0.1 s tick of StockGuard: hide while a live screen claims
\* the part, restore otherwise. A host that keeps its own guard runs the
\* same two functions for its own claims.
\*
\* A panel is drawn by sheets when a named HUD part claims it: the game
\* shows it only by its classes and writes no inline value on it, as a
\* sweep of the game's interface and its style sheets show. Any inline value
\* on such a panel is a script's, such as a mod's own visible write or an
\* earlier release's, and it outlives the script that wrote it. Other
\* panels the whole-HUD walk reaches, such as the hero shop, the game hides
\* by an inline collapse.
\*
\* Design "layer" is the guard as built: hide writes the inline collapse.
\* On a panel drawn by sheets it marks "sheets", and restore clears the
\* inline value through visible, as clearVisibility in src/visibility.ts does.
\* On any other panel it records in the mark the inline value it replaced,
\* again on every tick that finds another writer's value there, and restore
\* writes the record back. Design "visible" is the guard before it, which wrote visible false
\* on a panel it saw drawn, marked "shown", and wrote visible true for that
\* mark on restore.
\*
\* The one write the layer cannot see: an inline collapse the game writes
\* while the guard's own collapse stands looks the same as the guard's, so
\* GameWrite leaves it out; the restore then writes the older value back.
EXTENDS Naturals

CONSTANT Design

Layer == Design = "layer"

\* BySheets is the guard handing the panel to its sheets on restore.
BySheets == sheets

\* Through is the guard's clear writing visible before null, so the value a
\* panel keeps where its sheets name none is the default.
Through == TRUE

\* Inline is an inline style.visibility: unset, "visible" or "collapse".
Inline == {"none", "visible", "collapse"}

\* A mark is no mark, a recorded inline value, "sheets", or an earlier
\* release's "shown" or "unseen", which an earlier guard wrote over an unset
\* value.
Marks == {"unmarked", "sheets", "shown", "unseen"} \cup Inline

VARIABLES
  wish,     \* the style sheets draw the panel under the HUD's classes now
  named,    \* the sheets name a visibility for the panel at all
  drawn,    \* the panel is drawn: what the player sees
  game,     \* the inline value the game itself last wrote
  inline,   \* the panel's inline value, written by the game and the guard
  mark,     \* the guard's mark on the panel
  claimed,  \* the guard is allowed and a live screen claims the panel
  sheets,   \* the panel is drawn by sheets, as a named part's panels are
  given     \* the guard gave the panel back and no script wrote it since

vars == <<wish, named, drawn, game, inline, mark, claimed, sheets, given>>

\* Paint is what the panel draws once its inline value is i: that value, else
\* the sheets' wish where they name one, else the default visible when the
\* clear went through visible, else what it drew before.
Paint(i, via) ==
  IF i # "none" THEN i = "visible"
  ELSE IF named THEN wish
  ELSE IF via THEN TRUE ELSE drawn

\* Drawn is what the player sees.
Drawn == drawn

\* Init starts from the game's panel, or from an earlier release's mark on a
\* panel that release collapsed and the game never wrote. A panel drawn by
\* sheets may start with any script's inline value, and with any mark an
\* earlier guard left over its collapse. A panel whose sheets name nothing
\* may start with either value kept, as one an earlier release collapsed and
\* cleared keeps collapse.
Init ==
  /\ wish \in BOOLEAN
  /\ named \in BOOLEAN
  /\ claimed = FALSE
  /\ given = FALSE
  /\ sheets \in BOOLEAN
  /\ \/ /\ ~sheets
        /\ \/ game \in Inline /\ inline = game /\ mark = "unmarked"
           \/ game = "none" /\ inline = "collapse" /\ mark \in {"shown", "unseen"}
     \/ /\ sheets /\ game = "none"
        /\ \/ inline \in Inline /\ mark = "unmarked"
           \/ inline = "collapse" /\ mark \in Marks \ {"unmarked"}
  /\ drawn \in BOOLEAN
  /\ inline # "none" \/ named => drawn = Paint(inline, FALSE)

\* Style is the game's classes changing, which changes the sheets' wish.
Style ==
  /\ wish' \in BOOLEAN
  /\ drawn' = IF inline = "none" /\ named THEN wish' ELSE drawn
  /\ UNCHANGED <<named, game, inline, mark, claimed, sheets, given>>

\* GameWrite is the game writing the panel's inline value, which it does
\* only on a panel not drawn by sheets.
GameWrite ==
  /\ ~sheets
  /\ \E v \in Inline:
       /\ mark # "unmarked" => v # "collapse"
       /\ game' = v /\ inline' = v /\ drawn' = Paint(v, FALSE)
  /\ UNCHANGED <<wish, named, mark, claimed, sheets, given>>

\* ScriptWrite is a script other than the guard writing the inline value of
\* a panel drawn by sheets, such as a mod's visible write. A write while the
\* guard's mark stands is the mod's while it holds the part, so it goes at
\* the restore; one after the restore is the script's own again.
ScriptWrite ==
  /\ sheets
  /\ \E v \in Inline: inline' = v /\ drawn' = Paint(v, FALSE)
  /\ given' = FALSE
  /\ UNCHANGED <<wish, named, game, mark, claimed, sheets>>

\* Claim and Unclaim are everything that turns the guard's want for this
\* panel: a screen, the lease, a leave, the hideout.
Claim ==
  /\ claimed' = TRUE
  /\ UNCHANGED <<wish, named, drawn, game, inline, mark, sheets, given>>
Unclaim ==
  /\ claimed' = FALSE
  /\ UNCHANGED <<wish, named, drawn, game, inline, mark, sheets, given>>

\* Before is the inline value a mark records: nothing for "sheets" and for
\* an earlier release's marks.
Before(m) == IF m \in {"sheets", "shown", "unseen"} THEN "none" ELSE m

\* Hide is hidePanel.
Hide ==
  IF Layer
  THEN /\ mark' = IF BySheets THEN "sheets"
                  ELSE IF inline # "collapse" THEN inline
                  ELSE IF mark = "unmarked" THEN "collapse"
                  ELSE IF mark \in {"shown", "unseen"} THEN Before(mark)
                  ELSE mark
       /\ inline' = "collapse"
       /\ drawn' = FALSE
  ELSE /\ mark' = IF Drawn THEN "shown"
                  ELSE IF mark = "unmarked" THEN "unseen" ELSE mark
       /\ inline' = IF Drawn THEN "collapse" ELSE inline
       /\ drawn' = Paint(inline', FALSE)

\* Restore is restorePanel. A panel drawn by sheets is handed back to them,
\* through visible even when a script cleared the guard's collapse already.
\* On any other panel an inline value other than the guard's collapse is a
\* later writer's, so it stands.
Restore ==
  IF Layer
  THEN LET cleared == mark # "unmarked" /\ (BySheets \/ inline = "collapse")
       IN /\ inline' = IF mark = "unmarked" THEN inline
                       ELSE IF BySheets THEN "none"
                       ELSE IF inline # "collapse" THEN inline
                       ELSE Before(mark)
          /\ drawn' = Paint(inline', Through /\ cleared /\ inline' = "none")
          /\ mark' = "unmarked"
  ELSE /\ inline' = IF mark = "shown" THEN "visible"
                    ELSE IF inline = "collapse" THEN "none" ELSE inline
       /\ drawn' = Paint(inline', FALSE)
       /\ mark' = "unmarked"

\* Tick is the guard's 0.1 s reconcile for this panel.
Tick ==
  /\ IF claimed THEN Hide ELSE Restore
  /\ given' = IF claimed THEN FALSE ELSE given \/ mark # "unmarked"
  /\ UNCHANGED <<wish, named, game, claimed, sheets>>

Next == Style \/ GameWrite \/ ScriptWrite \/ Claim \/ Unclaim \/ Tick

\* Only the guard's tick always runs; the game writes when it likes.
Spec == Init /\ [][Next]_vars /\ WF_vars(Tick)

TypeOK ==
  /\ wish \in BOOLEAN /\ named \in BOOLEAN /\ drawn \in BOOLEAN
  /\ game \in Inline /\ inline \in Inline
  /\ mark \in Marks /\ claimed \in BOOLEAN
  /\ sheets \in BOOLEAN /\ given \in BOOLEAN

\* Exact: with no mark on it, a panel the game writes holds the game's
\* inline value, so it draws as the game says under every later class
\* change.
Exact == ~sheets /\ mark = "unmarked" => inline = game

\* Handed: once the guard gives back a panel drawn by sheets, no inline value
\* stands on it until a script writes one, so it draws as its classes say,
\* and as the default, visible, where they name nothing: a value a script
\* left before the hide, or wrote during it, is gone.
Handed == sheets /\ given => inline = "none" /\ drawn = (named => wish)

\* Recorded: while the guard's collapse stands under a mark, the mark holds
\* the game's value, which a restore writes back. A game write the next tick
\* has not met yet stands in the inline value itself.
Recorded == ~sheets /\ mark \in Inline /\ inline = "collapse" => mark = game

\* Hides: a claimed panel stops drawing within a tick, however often the game
\* writes it.
Hides == [](claimed => <>(~Drawn \/ ~claimed))

\* Returns: once nothing claims the panel, its mark goes within a tick, and
\* with it every write of the guard's.
Returns == [](~claimed => <>(mark = "unmarked" \/ claimed))
====
