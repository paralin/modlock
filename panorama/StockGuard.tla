---- MODULE StockGuard ----
\* The game's HUD panels persist from game to game, so a part a mod's screen
\* hides, a style it sets or a handler it leaves on a game panel outlives the
\* mod's session unless something undoes it. This spec follows the renderer's
\* writes on the game's panels across every way a player leaves a Modlock
\* game, and checks that the game's own HUD comes back however they leave,
\* without the mod doing anything.
\*
\* Callers, from source: panorama/src/main.ts hands the StockGuard
\* (src/guard.ts) the parts each screen the Bridge reads (bridge.ts) claims,
\* the relay's keepalive, and the game's disconnect and shutdown events. The
\* relay (session/relay.go) streams the screen of the local player:
\* Screens.Screen on the host, the Mirror of Screens.Player on a guest. Both
\* are empty once the server no longer lists the player, and a guest's Mirror
\* is cleared when its stream from the host is lost (peernet/host.go watch).
\* Hyperline's resident embeds the same Drawing and is left out here: its own
\* StockHud.tla covers its lease, and it runs this guard's HudParts, the same
\* hide and restore, under that lease.
\*
\* Design "edge" is Modlock's Hud before the guard: it hides when a screen lists a part,
\* sweeps every 0.1 s, and shows a part again only when a later screen, or the
\* null of a relay that ended, drops it, from its own memory of what it hid.
\* Design "guard" is the stock guard: one writer that every 0.1 s makes the
\* panels match what the live screen claims while the guard is allowed, and
\* restores everything its marks record while it is not. Allowed is a level:
\* a title within the lease, no disconnect or shutdown event since the last
\* screen, and no hideout connection. The relay sends a short keepalive title
\* every 5 s while the server lists the player, so silence means a dead
\* stream or a player the server let go.
\*
\* Panels are one abstract part: hidden is whether any of the guard's marks
\* stand on a game panel. The lease's clock is the Expire action; the 0.1 s
\* tick is Tick. A /restartmod stops the mod and starts it again, which the
\* screen shows as a screen without claims and then one with them, both
\* Screen steps.
EXTENDS Naturals

CONSTANT Design

\* Edge is the design the code ran before the guard. No config selects
\* "edge"; its mutant shows the rule it breaks.
Edge == Design = "edge"

Games == {"mod", "hideout", "other"}
Owners == {"live", "dead"}

VARIABLES
  game,     \* where the client is: the mod's server, the hideout, any other game
  listed,   \* the server lists the player and the relay delivers its titles
  claims,   \* the last screen the renderer read claims game parts, the
            \* ability bar's clicks (a root's ability_bar_presses) or the
            \* drawing of replaced abilities (a root's ability_display)
  late,     \* a claiming screen sent before the player left is still on its way
  fresh,    \* a title arrived within the lease (30 s)
  left,     \* a disconnect or shutdown event fired since the last screen
  hidden,   \* the renderer's writes stand on the game's panels: hidden
            \* parts, and Tab and bar icons the ability display drew
  memory,   \* the running script remembers hiding them
  handlers, \* click handlers on the ability bar and hover handlers on the
            \* display's covers over Tab's icons, by the script that left them
  barred    \* a handler sent a press or opened a window while not allowed

vars == <<game, listed, claims, late, fresh, left, hidden, memory, handlers, barred>>

\* Allowed is when the game's HUD may carry the screen's writes: what the
\* guard computes, and what every design must respect.
Allowed == fresh /\ ~left /\ game # "hideout"

\* Want is what the guard's tick makes the panels show.
Want == Allowed /\ claims

\* Marked is what a restore can see: the guard's marks on the panels, which
\* outlive the script that wrote them. The guard keeps no memory of its own;
\* the edge design's Hud does. The whole HUD's marks are found by a walk the
\* restore makes only while a mark on the HUD itself stands, which a hide sets
\* first and a restore drops last, so every mark stays in sight.
Marked == hidden

\* Gate is whether a handler on a game panel acts: the guard's handlers act
\* only for the guard instance named in the gate attribute, and only while it
\* is allowed. In code the attribute is BAR_GATE on the game's HUD, which
\* each StockGuard sets to its own token as it starts, and a handler on the
\* ability bar or a display cover checks it and allowed() before it sends a
\* press or opens the game's ability window.
Gate(h) == h = "live" /\ Allowed

Init == /\ game = "hideout" /\ listed = FALSE
        /\ claims = FALSE /\ late = FALSE /\ fresh = FALSE /\ left = FALSE
        /\ hidden = FALSE /\ memory = FALSE /\ handlers = {} /\ barred = FALSE

\* Draw is the edge design's Hud.draw for a screen that claims c: it hides
\* what the screen lists and shows what it hid before and no longer lists.
\* The guard design draws nothing on a screen; its tick does.
Draw(c) ==
  IF Edge
    THEN /\ hidden' = (c \/ (hidden /\ ~memory))
         /\ memory' = c
    ELSE UNCHANGED <<hidden, memory>>

\* Join connects to the mod's server, which lists the player. Leaving the old
\* map fires GameSystemGameShutdown, so the guard waits for a first screen.
Join ==
  /\ game # "mod" /\ game' = "mod" /\ listed' = TRUE
  /\ left' = TRUE /\ late' = FALSE
  /\ UNCHANGED <<claims, fresh, hidden, memory, handlers, barred>>

\* Screen is a screen read whole from the relay. A server that no longer
\* lists the player sends it an empty screen.
Screen ==
  /\ \E c \in BOOLEAN:
       /\ listed \/ ~c
       /\ claims' = c /\ fresh' = TRUE /\ left' = FALSE
       /\ Draw(c)
  /\ UNCHANGED <<game, listed, late, handlers, barred>>

\* LateScreen is a claiming screen the server sent before it let the player
\* go, read after the player left: the late load.
LateScreen ==
  /\ late /\ late' = FALSE
  /\ claims' = TRUE /\ fresh' = TRUE /\ left' = FALSE
  /\ Draw(TRUE)
  /\ UNCHANGED <<game, listed, handlers, barred>>

\* Keepalive is the relay's 5 s status title while the stream is quiet. It
\* keeps the lease and clears no leave event. The screen says whether the
\* server lists the player, and the relay sends a changed screen before the
\* next keepalive, so once the player is let go no keepalive comes before
\* the screen without claims.
Keepalive ==
  /\ (listed \/ ~claims) /\ ~fresh /\ fresh' = TRUE
  /\ UNCHANGED <<game, listed, claims, late, left, hidden, memory, handlers, barred>>

\* Settled is the guard with no tick due: the 0.1 s tick always runs before a
\* 30 s lease ends.
Settled ==
  \/ Edge
  \/ /\ hidden = Want \/ (~Want /\ ~Marked)
     /\ Want => "live" \in handlers

\* Expire is 30 s without a title.
Expire ==
  /\ fresh /\ ~listed /\ Settled /\ fresh' = FALSE
  /\ UNCHANGED <<game, listed, claims, late, left, hidden, memory, handlers, barred>>

\* Stall stops the titles of a player still on the server: a stalled
\* controller, or a relay page lost to the game. Resume is the page loaded
\* again, which reads the screen whole before anything else.
Stall ==
  /\ game = "mod" /\ listed /\ listed' = FALSE
  /\ UNCHANGED <<game, claims, late, fresh, left, hidden, memory, handlers, barred>>
Resume ==
  /\ game = "mod" /\ ~listed /\ listed' = TRUE
  /\ UNCHANGED <<game, claims, late, fresh, left, hidden, memory, handlers, barred>>

\* StopSession is the controller stopping or crashing: the relay's stream
\* ends, its page sets an empty title, and the Bridge shows null.
StopSession ==
  /\ listed /\ listed' = FALSE /\ late' = FALSE
  /\ claims' = FALSE /\ Draw(FALSE)
  /\ UNCHANGED <<game, fresh, left, handlers, barred>>

\* Leave goes from where the client is to another game or the hideout, by
\* the menu, a queue or a console connect, with the disconnect or shutdown
\* event the renderer hears. A claiming screen may still be on its way.
Leave ==
  /\ \E to \in Games \ {"mod", game}, l \in BOOLEAN:
       /\ game' = to /\ left' = TRUE /\ late' = (l /\ listed /\ claims)
  /\ UNCHANGED <<listed, claims, fresh, hidden, memory, handlers, barred>>

\* LeaveUnseen reaches another game with no event the renderer hears: the
\* server that just goes away, or an event lost to a script mid-reload.
LeaveUnseen ==
  /\ game = "mod" /\ game' = "other"
  /\ \E l \in BOOLEAN: late' = (l /\ listed /\ claims)
  /\ UNCHANGED <<listed, claims, fresh, left, hidden, memory, handlers, barred>>

\* ServerNotices is the mod's server letting a player go who is no longer
\* on it: at once on a disconnect, after its timeout on a vanished client.
ServerNotices ==
  /\ game # "mod" /\ listed /\ listed' = FALSE
  /\ UNCHANGED <<game, claims, late, fresh, left, hidden, memory, handlers, barred>>

\* Reload is the renderer's script starting again with a fresh page and no
\* memory: a remounted package or a reloaded interface. What the old script
\* wrote on the game's panels stays, and its handlers stay, now dead.
Reload ==
  /\ memory' = FALSE /\ claims' = FALSE /\ fresh' = FALSE /\ left' = FALSE
  /\ late' = FALSE
  /\ handlers' = IF handlers = {} THEN {} ELSE {"dead"}
  /\ UNCHANGED <<game, listed, hidden, barred>>

\* Rebuild is the game rebuilding a hidden panel, which shows it again.
Rebuild ==
  /\ hidden /\ hidden' = FALSE
  /\ UNCHANGED <<game, listed, claims, late, fresh, left, memory, handlers, barred>>

\* Sweep is the edge design's 0.1 s sweep, hiding again what it remembers.
Sweep ==
  /\ Edge /\ memory /\ ~hidden /\ hidden' = TRUE
  /\ UNCHANGED <<game, listed, claims, late, fresh, left, memory, handlers, barred>>

\* Tick is the guard's 0.1 s reconcile: it hides and draws what is wanted,
\* restores what the marks record otherwise, and adds its handlers to the
\* ability bar and its covers to Tab while a wanted screen asks for them. A
\* display cover is the guard's own panel, which a restore deletes.
Tick ==
  /\ ~Edge
  /\ hidden' = IF Want THEN TRUE ELSE IF Marked THEN FALSE ELSE hidden
  /\ memory' = memory
  /\ handlers' = IF Want THEN handlers \cup {"live"} ELSE handlers
  /\ <<hidden', handlers'>> # <<hidden, handlers>>
  /\ UNCHANGED <<game, listed, claims, late, fresh, left, barred>>

\* Press is a click or a hover on a game panel that carries a handler.
Press ==
  /\ \E h \in handlers: Gate(h)
  /\ barred' = (barred \/ ~Allowed)
  /\ UNCHANGED <<game, listed, claims, late, fresh, left, hidden, memory, handlers>>

Next == Join \/ Screen \/ LateScreen \/ Keepalive \/ Expire \/ Stall \/ Resume
        \/ StopSession \/ Leave \/ LeaveUnseen \/ ServerNotices \/ Reload
        \/ Rebuild \/ Sweep \/ Tick \/ Press

\* The guard's tick and the lease's clock always run, and the server lets
\* a player go who left.
Spec == Init /\ [][Next]_vars
        /\ WF_vars(Tick) /\ WF_vars(Sweep) /\ WF_vars(Expire) /\ WF_vars(ServerNotices)

\* HideoutImmediate: nothing is allowed in the hideout, without waiting for
\* the lease.
HideoutImmediate == game = "hideout" => ~Allowed

\* NoHideWhileBarred: no write lands on the game's panels in a state that
\* does not allow it.
NoHideWhileBarred == [][~Allowed' => (hidden' => hidden)]_vars

\* PressOnlyWhileAllowed: no handler on a game panel sends a press while not
\* allowed, whichever script left it.
PressOnlyWhileAllowed == ~barred

\* LeaveWithoutLease: after a disconnect or shutdown event the game's HUD is
\* back before the lease ends; the lease only covers what no event reports.
LeaveWithoutLease == [][fresh /\ ~fresh' /\ left' => ~hidden]_vars

\* HudReturns: whatever was written and whichever script wrote it, the game's
\* HUD comes back whenever the screen may not hold it, unless it may again.
HudReturns == [](hidden /\ ~Allowed => <>(~hidden \/ Allowed))

\* LeaveReturns: however the player leaves the mod's server, the game's HUD
\* comes back unless they return to it.
LeaveReturns == [](game # "mod" => <>(~hidden \/ game = "mod"))
====
