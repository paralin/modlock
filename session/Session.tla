---- MODULE Session ----
\* A play session, as session.go runs it: Start mounts the mods' interfaces and
\* content, starts the host and serves the control link; the first Ready event
\* launches the Deadlock client; Stop, which an ended Start context also calls,
\* unmounts and then ends the host; and Wait waits for the host, closes the
\* link and the interfaces, and waits for the link's last events.
\*
\* Each goroutine's step is one action: the link's reader (Connect, Handle,
\* LinkEnds, LinkCanceled), Stop's steps, and Wait's. Not modeled: reloads,
\* which queue until the host connects; the screen server; and Proton.

EXTENDS Naturals

VARIABLES
  host,      \* "starting", "up" once it dials the link, "dead" once it exits
  link,      \* "waiting" for the host, "connected", or "closed"
  event,     \* a Ready event is on the link and not yet handled
  ready,     \* the host sent Ready
  mounted,   \* the mods' package is mounted in the player's game
  once,      \* stopInterface has run; sync.OnceFunc runs it once
  unmounts,  \* how many times the package was unmounted
  launched,  \* set when the client launched or may no longer launch
  client,    \* the Deadlock client was launched
  ctx,       \* Start's context ended
  pending,   \* ctx's AfterFunc fired and its Stop has not begun
  stopPc,    \* Stop's step: "idle", "unmount", "kill", "done"
  waitPc,    \* Wait's step: "host", "close", "drain", "done"
  killedMounted,     \* Stop ended the host while the package was mounted
  launchedAfterStop  \* the client launched after Stop began
vars == <<host, link, event, ready, mounted, once, unmounts, launched, client,
          ctx, pending, stopPc, waitPc, killedMounted, launchedAfterStop>>

Stopped == stopPc # "idle"

Init ==
  /\ host = "starting" /\ link = "waiting" /\ event = FALSE /\ ready = FALSE
  /\ mounted = TRUE /\ once = FALSE /\ unmounts = 0
  /\ launched = FALSE /\ client = FALSE /\ ctx = FALSE /\ pending = FALSE
  /\ stopPc = "idle" /\ waitPc = "host"
  /\ killedMounted = FALSE /\ launchedAfterStop = FALSE

\* stopInterface: Served.Stop behind sync.OnceFunc.
Unmount ==
  IF once THEN UNCHANGED <<mounted, once, unmounts>>
  ELSE mounted' = FALSE /\ once' = TRUE /\ unmounts' = unmounts + 1

\* The host dials the control link and Run accepts it.
Connect ==
  /\ host = "starting" /\ link = "waiting"
  /\ host' = "up" /\ link' = "connected"
  /\ UNCHANGED <<event, ready, mounted, once, unmounts, launched, client, ctx,
                 pending, stopPc, waitPc, killedMounted, launchedAfterStop>>

HostReady ==
  /\ host = "up" /\ link = "connected" /\ ~ready
  /\ ready' = TRUE /\ event' = TRUE
  /\ UNCHANGED <<host, link, mounted, once, unmounts, launched, client, ctx,
                 pending, stopPc, waitPc, killedMounted, launchedAfterStop>>

\* handle: the first Ready swaps launched and launches the client, which
\* fails to start once ctx has ended.
Handle ==
  /\ event /\ link = "connected"
  /\ event' = FALSE
  /\ launched' = TRUE
  /\ client' = (client \/ (~launched /\ ~ctx))
  /\ launchedAfterStop' = (launchedAfterStop \/ (~launched /\ ~ctx /\ Stopped))
  /\ UNCHANGED <<host, link, ready, mounted, once, unmounts, ctx, pending,
                 stopPc, waitPc, killedMounted>>

\* The host exits on its own, or crashes.
Exit ==
  /\ host # "dead"
  /\ host' = "dead"
  /\ UNCHANGED <<link, event, ready, mounted, once, unmounts, launched, client,
                 ctx, pending, stopPc, waitPc, killedMounted, launchedAfterStop>>

\* Run reads the end of the connection once the host is gone.
LinkEnds ==
  /\ host = "dead" /\ link = "connected" /\ ~event
  /\ link' = "closed"
  /\ UNCHANGED <<host, event, ready, mounted, once, unmounts, launched, client,
                 ctx, pending, stopPc, waitPc, killedMounted, launchedAfterStop>>

\* Start's context ends. Until Wait stops it, its AfterFunc calls Stop.
Cancel ==
  /\ ~ctx
  /\ ctx' = TRUE
  /\ pending' = (waitPc = "host")
  /\ UNCHANGED <<host, link, event, ready, mounted, once, unmounts, launched,
                 client, stopPc, waitPc, killedMounted, launchedAfterStop>>

\* Run's AfterFuncs close the listener and the connection once ctx ends.
LinkCanceled ==
  /\ ctx /\ link # "closed"
  /\ link' = "closed" /\ event' = FALSE
  /\ UNCHANGED <<host, ready, mounted, once, unmounts, launched, client, ctx,
                 pending, stopPc, waitPc, killedMounted, launchedAfterStop>>

\* Stop begins, called directly, as waitSession does when the player ends the
\* session, or by ctx's AfterFunc. It first sets launched, so no later Ready
\* launches the client.
StopBegin ==
  /\ ~Stopped
  /\ stopPc' = "unmount" /\ pending' = FALSE /\ launched' = TRUE
  /\ UNCHANGED <<host, link, event, ready, mounted, once, unmounts, client, ctx,
                 waitPc, killedMounted, launchedAfterStop>>

StopPending == pending /\ StopBegin

StopUnmount ==
  /\ stopPc = "unmount"
  /\ Unmount
  /\ stopPc' = "kill"
  /\ UNCHANGED <<host, link, event, ready, launched, client, ctx, pending,
                 waitPc, killedMounted, launchedAfterStop>>

StopKill ==
  /\ stopPc = "kill"
  /\ host' = "dead" /\ stopPc' = "done"
  /\ killedMounted' = (killedMounted \/ mounted)
  /\ UNCHANGED <<link, event, ready, mounted, once, unmounts, launched, client,
                 ctx, pending, waitPc, launchedAfterStop>>

\* Wait: once the host exits, stop ctx's AfterFunc and close the listener.
WaitHost ==
  /\ waitPc = "host" /\ host = "dead"
  /\ waitPc' = "close"
  /\ link' = IF link = "waiting" THEN "closed" ELSE link
  /\ UNCHANGED <<host, event, ready, mounted, once, unmounts, launched, client,
                 ctx, pending, stopPc, killedMounted, launchedAfterStop>>

\* Then close the interfaces.
WaitClose ==
  /\ waitPc = "close"
  /\ Unmount
  /\ waitPc' = "drain"
  /\ UNCHANGED <<host, link, event, ready, launched, client, ctx, pending,
                 stopPc, killedMounted, launchedAfterStop>>

\* Then wait for Run to return.
WaitDrain ==
  /\ waitPc = "drain" /\ link = "closed"
  /\ waitPc' = "done"
  /\ UNCHANGED <<host, link, event, ready, mounted, once, unmounts, launched,
                 client, ctx, pending, stopPc, killedMounted, launchedAfterStop>>

Done == waitPc = "done" /\ UNCHANGED vars

Progress ==
  \/ Handle \/ LinkEnds \/ LinkCanceled \/ StopPending \/ StopUnmount
  \/ StopKill \/ WaitHost \/ WaitClose \/ WaitDrain
Next == Progress \/ Connect \/ HostReady \/ Exit \/ Cancel \/ StopBegin \/ Done
Spec == Init /\ [][Next]_vars /\ WF_vars(Progress)

\* The package is unmounted at most once, so a second unmount never removes a
\* block a later session wrote.
UnmountOnce == unmounts <= 1
\* Stop unmounts before it ends the host, since the game holds gameinfo.gi
\* once its server stops.
UnmountBeforeKill == ~killedMounted
\* The client starts only after the server is ready, and never once Stop has
\* begun.
LaunchAfterReady == client => ready
NoLaunchAfterStop == ~launchedAfterStop
\* A stopped session, or one whose host exits, reaches the end of Wait with
\* the package unmounted.
Ends == (Stopped \/ host = "dead") ~> (waitPc = "done" /\ ~mounted)
====
