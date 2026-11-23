---- MODULE LayoutUnload ----
\* A layout's script registers cleanup with Modlock.onUnload and must hear
\* about its panel leaving before the panel, and with it the script, is gone.
\* This spec follows one loaded layout panel through the renderer's
\* notifyUnloading, DeleteAsync and the frame that destroys the panel, and the
\* layout library's reread, onUnload and unload, and checks that each cleanup
\* runs once, before the script dies, and that no data reaches a layout after
\* its notice.
\*
\* Callers, from source: panorama/src/draw.ts calls notifyUnloading before
\* every DeleteAsync of a loaded layout (a dropped mod, a new generation, a
\* node that left the tree, a replaced panel and its subtree) and in release,
\* which marks without deleting; the next draw then replaces each marked panel
\* because kept requires !old.unloading. notifyUnloading sets the
\* modlock_unloading attribute and dispatches the PlaySoundEffect wake once.
\* panorama/src/layout.ts rereads on each wake: an unloaded script ignores it,
\* a leaving panel (marked, or no longer valid) runs every cleanup once and
\* stops the data watches, and otherwise each changed attribute reaches its
\* watch. onUnload on a leaving panel runs its handler at once.
\*
\* Delivery is when the client runs the wake's listeners: "sync" inside
\* DispatchEvent, "queued" at some later point. Outlives is whether a layout's
\* script keeps running after the client destroys its panel. Neither is
\* measured in the client yet. DeleteAsync(0) destroys the panel on a later
\* frame, so under "sync" the notice lands while the panel is valid. Under
\* "queued" the design holds only when the script outlives its panel, through
\* the IsValid check on the late wake; the mutants show the other case. With
\* the attribute always set first, that IsValid check matters only for a
\* panel deleted without a notice; it keeps such a layout's cleanup running on
\* the next wake when the script outlives its panel.
\*
\* One panel and one registered handler bound the state space; a nested
\* layout is a second copy of the same lifecycle, and the renderer tests cover
\* that subtree marks every nested layout.
EXTENDS Naturals
CONSTANTS Delivery, Outlives

\* panel: "none" before the draw makes it, "live" while drawn, "deleting"
\* after DeleteAsync, "gone" once the client destroys it.
\* marked: the renderer set modlock_unloading on it.
\* script: "none", "running", "unloaded" after its cleanups ran, "dead" once
\* destroyed with its panel. listening: it called data or onUnload, which
\* register the library's wake listener.
\* registered: the script called onUnload; handler: that handler waits to run.
\* ran: how many times it ran. pending: a wake waits for delivery.
\* fresh: data waits to be read. stale: data reached a layout after its notice.
\* lost: a script died before its registered cleanup ran.
VARIABLES panel, marked, script, listening, registered, handler, ran, pending,
          fresh, stale, lost
vars == <<panel, marked, script, listening, registered, handler, ran, pending,
          fresh, stale, lost>>

Init == /\ panel = "none" /\ marked = FALSE /\ script = "none"
        /\ listening = FALSE /\ registered = FALSE /\ handler = FALSE
        /\ ran = 0 /\ pending = FALSE /\ fresh = FALSE /\ stale = FALSE
        /\ lost = FALSE

\* leaving() in layout.ts, seen with panel state p: the attribute is set, or
\* the panel is no longer valid.
Leaving(p) == marked' \/ p = "gone"

\* Reread(p) is one delivered wake: the library's reread with panel state p.
\* It leaves panel, marked, listening and registered to its caller.
Reread(p) ==
  /\ pending' = FALSE
  /\ lost' = lost
  /\ IF script # "running" \/ ~listening
     THEN UNCHANGED <<script, handler, ran, fresh, stale>>
     ELSE IF Leaving(p)
     THEN /\ script' = "unloaded" /\ handler' = FALSE
          /\ ran' = ran + (IF handler THEN 1 ELSE 0)
          /\ UNCHANGED <<fresh, stale>>
     ELSE /\ fresh' = FALSE /\ stale' = (stale \/ (fresh /\ marked'))
          /\ UNCHANGED <<script, handler, ran>>

\* The draw creates the panel and loads the layout, whose script starts with
\* its data waiting.
Create ==
  /\ panel = "none"
  /\ panel' = "live" /\ script' = "running" /\ fresh' = TRUE
  /\ UNCHANGED <<marked, listening, registered, handler, ran, pending, stale,
                 lost>>

\* The script calls data, which listens for wakes.
Watch ==
  /\ script = "running" /\ ~listening
  /\ listening' = TRUE
  /\ UNCHANGED <<panel, marked, script, registered, handler, ran, pending,
                 fresh, stale, lost>>

\* The script calls onUnload, once; on a leaving panel the handler runs now.
Register ==
  /\ script \in {"running", "unloaded"} /\ ~registered
  /\ registered' = TRUE /\ listening' = TRUE
  /\ IF script = "unloaded" \/ marked \/ panel = "gone"
     THEN /\ script' = "unloaded" /\ ran' = ran + 1
          /\ UNCHANGED handler
     ELSE /\ handler' = TRUE
          /\ UNCHANGED <<script, ran>>
  /\ UNCHANGED <<panel, marked, pending, fresh, stale, lost>>

\* A redraw keeps an unmarked panel and hands it new data with a wake.
Redraw ==
  /\ panel = "live" /\ ~marked
  /\ fresh' = TRUE /\ pending' = TRUE
  /\ UNCHANGED <<panel, marked, script, listening, registered, handler, ran,
                 stale, lost>>

\* Another layout's data or notice wakes every layout, at any time.
Wake ==
  /\ ~pending /\ pending' = TRUE
  /\ UNCHANGED <<panel, marked, script, listening, registered, handler, ran,
                 fresh, stale, lost>>

\* notifyUnloading: mark the live panel and wake the library.
Mark ==
  /\ panel = "live" /\ ~marked
  /\ marked' = TRUE
  /\ IF Delivery = "sync"
     THEN Reread(panel)
     ELSE /\ pending' = TRUE
          /\ UNCHANGED <<script, handler, ran, fresh, stale, lost>>
  /\ UNCHANGED <<panel, listening, registered>>

\* DeleteAsync, which the renderer calls only after marking.
Delete ==
  /\ panel = "live" /\ marked
  /\ panel' = "deleting"
  /\ UNCHANGED <<marked, script, listening, registered, handler, ran, pending,
                 fresh, stale, lost>>

\* A later frame destroys the panel, and the script with it unless it
\* outlives its panel.
Destroy ==
  /\ panel = "deleting"
  /\ panel' = "gone"
  /\ script' = IF Outlives THEN script ELSE "dead"
  /\ lost' = (lost \/ (registered /\ ran = 0 /\ script' = "dead"))
  /\ UNCHANGED <<marked, listening, registered, handler, ran, pending, fresh,
                 stale>>

\* The client delivers a wake; any layout's wake rereads every layout.
Deliver ==
  /\ pending
  /\ UNCHANGED <<panel, marked, listening, registered>>
  /\ Reread(panel)

Next == Create \/ Watch \/ Register \/ Redraw \/ Wake \/ Mark \/ Delete
        \/ Destroy \/ Deliver \/ UNCHANGED vars
Spec == Init /\ [][Next]_vars /\ WF_vars(Deliver) /\ WF_vars(Destroy)

\* Each cleanup runs at most once.
CleanupOnce == ran <= 1
\* No script dies with a cleanup still waiting.
CleanupBeforeDeath == ~lost
\* No data reaches a layout after its panel was marked.
NoDataAfterNotice == ~stale
\* A script whose cleanups ran never sits on a panel the renderer still shows.
UnloadedNotShown == script = "unloaded" => marked \/ panel = "gone"

\* A waiting cleanup on a leaving panel eventually runs.
NoticeRuns == [](handler /\ (marked \/ panel = "gone") => <>~handler)
====
