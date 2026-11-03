---- MODULE EngineHooks ----
\* The engine hooks a mod's features share, as engine_host.cc installs them and
\* wasm_plugin_events.cc subscribes to them. A hook installs on its first
\* subscriber and stays; the connection tracker serves chat, console commands
\* and departures; the combat hook serves damage and movement. An install can
\* fail for good, such as a signature a game update broke, and the mod plays on
\* without that feature.
\*
\* ListenToPlayers runs at each world and subscribes chat, console commands and
\* departures; ListenToCombat runs at the world when the mod wants damage, and
\* from each frame once the mod moves players. Not modeled: reloads, and an
\* install that fails only because its module has not loaded yet, which the
\* next world retries.
\*
\* SkipRest is the old ListenToPlayers, which returned at a failed chat hook
\* and never tried console commands or departures. RetryEveryFrame is the old
\* Observe, which tried a failed combat install again on every frame.
EXTENDS Naturals, FiniteSets
CONSTANTS SkipRest, RetryEveryFrame
VARIABLES s, broken, wants
vars == <<s, broken, wants>>

Hooks == {"tracker", "chat", "command", "combat"}
Features == {"chat", "command", "depart", "combat"}
Players == {"chat", "command", "depart"}
Needs(f) == CASE f = "chat" -> {"tracker", "chat"}
              [] f = "command" -> {"tracker", "command"}
              [] f = "depart" -> {"tracker"}
              [] f = "combat" -> {"combat"}

Init ==
  /\ broken \in SUBSET Hooks /\ wants \in BOOLEAN
  /\ s = [world |-> 0, moving |-> FALSE, watched |-> FALSE, frames |-> 0, tried |-> FALSE,
          hook |-> {}, installs |-> [h \in Hooks |-> 0],
          sub |-> {}, attempts |-> [f \in Features |-> 0]]

\* Try subscribes f: it installs the hooks f needs that work, and subscribes
\* only when all of them do. A failure is one attempt for f.
Try(t, f) ==
  LET ok == Needs(f) \cap broken = {}
      fresh == (Needs(f) \ broken) \ t.hook
  IN [t EXCEPT !.hook = @ \cup (Needs(f) \ broken),
               !.installs = [h \in Hooks |-> IF h \in fresh THEN @[h] + 1 ELSE @[h]],
               !.sub = IF ok THEN @ \cup {f} ELSE @,
               !.attempts = [@ EXCEPT ![f] = IF ok THEN @ ELSE @ + 1]]

ListenPlayers(t) ==
  IF SkipRest
  THEN IF "chat" \in t.sub THEN t
       ELSE LET a == Try(t, "chat")
            IN IF "chat" \in a.sub THEN Try(Try(a, "command"), "depart") ELSE a
  ELSE IF t.sub \cap Players # {} THEN t
       ELSE Try(Try(Try(t, "chat"), "command"), "depart")

ListenCombat(t) == IF "combat" \in t.sub THEN t ELSE Try(t, "combat")

\* The world is ready: new frames count afresh, and the plugin listens.
World ==
  /\ s.world < 2
  /\ LET t == [s EXCEPT !.world = @ + 1, !.frames = 0, !.tried = FALSE,
                        !.attempts = [f \in Features |-> 0]]
         p == ListenPlayers(t)
         c == ListenCombat(p)
     IN s' = IF wants THEN [c EXCEPT !.tried = "combat" \notin c.sub] ELSE p
  /\ UNCHANGED <<broken, wants>>

\* The mod starts moving players.
Move ==
  /\ s.world > 0 /\ ~s.moving
  /\ s' = [s EXCEPT !.moving = TRUE]
  /\ UNCHANGED <<broken, wants>>

\* Observe: a frame of a moving mod tries the combat hook while it has none.
Frame ==
  /\ s.world > 0 /\ s.frames < 3
  /\ LET t == [s EXCEPT !.frames = @ + 1, !.watched = @ \/ s.moving]
         try == s.moving /\ "combat" \notin s.sub /\ (RetryEveryFrame \/ ~s.tried)
         u == Try(t, "combat")
     IN s' = IF try THEN [u EXCEPT !.tried = "combat" \notin u.sub] ELSE t
  /\ UNCHANGED <<broken, wants>>

Next == World \/ Move \/ Frame \/ UNCHANGED vars
Spec == Init /\ [][Next]_vars

\* A hook installs at most once, so no event reaches a subscriber twice.
InstallOnce == \A h \in Hooks : s.installs[h] <= 1
\* A failed install is tried once for a world, not on every frame.
NoRetryStorm == \A f \in Features : s.attempts[f] <= 1
\* One failed hook disables only the features that need it: every other
\* feature the mod asks for is subscribed once the world is ready.
FailureIsolated ==
  s.world > 0 =>
    \A f \in Features :
      (Needs(f) \cap broken = {} /\ (f # "combat" \/ wants \/ s.watched))
        => f \in s.sub
\* Only a hook that works is installed.
OnlyWorkingHooks == s.hook \cap broken = {}
====
