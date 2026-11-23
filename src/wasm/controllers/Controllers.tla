---- MODULE Controllers ----
\* One player's controller stream, from a mod's watch to the state the mod
\* reads in each frame. The actors, and the code each stands for:
\*
\*   Mod     calls WatchControllers for the player, on and off.
\*   Host    counts each watch as a new epoch and reports the want with it,
\*           applies the frames of the current epoch from the bound stream,
\*           merges presses between mod frames, and marks the stream stale
\*           when its deadline passes with no frame.
\*   Feeder  the host's daemon or game server. It binds a stream to the peer
\*           that carries the player's game and relays the want to that peer.
\*           It keeps no state of its own beyond the binding, so it appears
\*           only in Connect, Lose and DeliverWant.
\*   Client  the player's reader. On each want epoch it opens the devices and
\*           sends their whole state, then changes and heartbeats; it closes
\*           the devices when the want ends or the link drops.
\*   Player  presses and releases a button, and moves the game's focus.
\*   Other   another peer that sends frames for this player.
\*
\* The stream is one reliable ordered stream, so frames are a sequence. Time
\* is abstract: the deadline may fire at any moment, and a frame applied since
\* it last fired rearms it.
EXTENDS Naturals, Sequences
CONSTANTS MaxEpoch, MaxConnects, MaxPresses, MaxInFlight

VARIABLES
  watched, epoch,      \* host: a mod watches the player, and the watch's epoch
  up, connects,        \* the bound peer link, and how many times it connected
  want, wantEpoch,     \* client: the last want the feeder relayed
  open, sentEpoch,     \* client: devices open, and the epoch its last full set answered
  held, focus,         \* player: the button, and whether the game has focus
  unsent, presses,     \* client: presses not sent yet; the player's total presses
  chan,                \* frames in flight on the bound stream
  synced, hHeld, hFocus, recent, stale, \* host: its copy of the stream
  pending, delivered, accepted, from    \* host: presses held, given, applied; whose frames
vars == <<watched, epoch, up, connects, want, wantEpoch, open, sentEpoch,
          held, focus, unsent, presses, chan, synced, hHeld, hFocus, recent,
          stale, pending, delivered, accepted, from>>

\* The state a mod reads in each frame.
State == IF ~watched \/ ~synced THEN "none"
         ELSE IF stale THEN "stale"
         ELSE IF ~hFocus THEN "unfocused"
         ELSE "live"

\* The client reports a held button only while the game has focus, so a
\* player who looks away does not leave a stick or trigger held.
Report == held /\ focus

\* The feeder accepts a stream only from the peer that carries the player's
\* game connection.
PeerCarriesGame(p) == p = "owner"

Frame(kind, e, n) ==
  [kind |-> kind, epoch |-> e, held |-> Report, focus |-> focus, presses |-> n]

Init ==
  /\ watched = FALSE /\ epoch = 0
  /\ up = FALSE /\ connects = 0
  /\ want = FALSE /\ wantEpoch = 0
  /\ open = FALSE /\ sentEpoch = 0
  /\ held = FALSE /\ focus \in BOOLEAN
  /\ unsent = 0 /\ presses = 0
  /\ chan = <<>>
  /\ synced = FALSE /\ hHeld = FALSE /\ hFocus = FALSE
  /\ recent = FALSE /\ stale = FALSE
  /\ pending = 0 /\ delivered = 0 /\ accepted = 0 /\ from = "none"

ClientVars == <<want, wantEpoch, open, sentEpoch, unsent>>
HostVars == <<synced, hHeld, hFocus, recent, stale, pending, delivered, accepted, from>>
PlayerVars == <<held, focus, presses>>

\* A mod calls WatchControllers: a new epoch, and nothing to show until the
\* client's full set for it arrives.
Watch ==
  /\ ~watched /\ epoch < MaxEpoch
  /\ watched' = TRUE /\ epoch' = epoch + 1
  /\ UNCHANGED <<up, connects, chan>> /\ UNCHANGED ClientVars
  /\ UNCHANGED PlayerVars /\ UNCHANGED HostVars

\* The mod stops watching, or stops. The host drops its copy and the presses
\* the mod has not taken.
Unwatch ==
  /\ watched
  /\ watched' = FALSE
  /\ synced' = FALSE /\ hHeld' = FALSE /\ stale' = FALSE /\ recent' = FALSE
  /\ pending' = 0 /\ accepted' = delivered
  /\ UNCHANGED <<epoch, up, connects, chan, hFocus, delivered, from>>
  /\ UNCHANGED ClientVars /\ UNCHANGED PlayerVars

\* The client's peer link comes up and the feeder binds its stream. A new
\* connection starts the client with no want.
Connect ==
  /\ ~up /\ connects < MaxConnects
  /\ up' = TRUE /\ connects' = connects + 1 /\ chan' = <<>>
  /\ want' = FALSE /\ wantEpoch' = 0 /\ sentEpoch' = 0 /\ unsent' = 0
  /\ UNCHANGED <<watched, epoch, open>> /\ UNCHANGED PlayerVars
  /\ UNCHANGED HostVars

\* The link drops or the client exits: frames in flight are lost, the client
\* closes its devices, and the host's copy goes with the stream, the presses
\* the mod has not taken included, since a stream that shows none lists no
\* device to carry them.
Lose ==
  /\ up
  /\ up' = FALSE /\ chan' = <<>> /\ open' = FALSE /\ want' = FALSE
  /\ synced' = FALSE /\ hHeld' = FALSE /\ stale' = FALSE /\ recent' = FALSE
  /\ UNCHANGED <<watched, epoch, connects, wantEpoch, sentEpoch, unsent>>
  /\ UNCHANGED PlayerVars
  /\ pending' = 0 /\ accepted' = delivered
  /\ UNCHANGED <<hFocus, delivered, from>>

\* The feeder relays the host's latest want. Wants coalesce, so a watch that
\* ends and starts again before a relay reaches the client as one new epoch.
DeliverWant ==
  /\ up /\ <<want, wantEpoch>> # <<watched, epoch>>
  /\ want' = watched /\ wantEpoch' = epoch
  /\ UNCHANGED <<watched, epoch, up, connects, open, sentEpoch, unsent, chan>>
  /\ UNCHANGED PlayerVars /\ UNCHANGED HostVars

\* The client answers each want epoch with the devices' whole state, opening
\* them if they were closed.
SendFull ==
  /\ up /\ want /\ sentEpoch # wantEpoch /\ Len(chan) < MaxInFlight
  /\ open' = TRUE /\ sentEpoch' = wantEpoch /\ unsent' = 0
  /\ chan' = Append(chan, Frame("full", wantEpoch, 0))
  /\ UNCHANGED <<watched, epoch, up, connects, want, wantEpoch>>
  /\ UNCHANGED PlayerVars /\ UNCHANGED HostVars

\* Then a change, or a heartbeat when nothing changed.
SendDelta ==
  /\ up /\ open /\ want /\ sentEpoch = wantEpoch /\ Len(chan) < MaxInFlight
  /\ chan' = Append(chan, Frame("delta", sentEpoch, unsent)) /\ unsent' = 0
  /\ UNCHANGED <<watched, epoch, up, connects, want, wantEpoch, open, sentEpoch>>
  /\ UNCHANGED PlayerVars /\ UNCHANGED HostVars

\* With no want, the client closes the devices.
Close ==
  /\ open /\ ~want
  /\ open' = FALSE /\ sentEpoch' = 0 /\ unsent' = 0
  /\ UNCHANGED <<watched, epoch, up, connects, want, wantEpoch, chan>>
  /\ UNCHANGED PlayerVars /\ UNCHANGED HostVars

Press ==
  /\ ~held /\ presses < MaxPresses
  /\ held' = TRUE /\ presses' = presses + 1
  /\ unsent' = IF open /\ focus THEN unsent + 1 ELSE unsent
  /\ UNCHANGED <<watched, epoch, up, connects, want, wantEpoch, open, sentEpoch>>
  /\ UNCHANGED <<focus, chan>> /\ UNCHANGED HostVars

Release ==
  /\ held /\ held' = FALSE
  /\ UNCHANGED <<watched, epoch, up, connects, chan, focus, presses>>
  /\ UNCHANGED ClientVars /\ UNCHANGED HostVars

Refocus ==
  /\ focus' = ~focus
  /\ UNCHANGED <<watched, epoch, up, connects, chan, held, presses>>
  /\ UNCHANGED ClientVars /\ UNCHANGED HostVars

\* The host applies the next frame between server frames. It keeps a frame of
\* the watch's own epoch, a delta only on top of that epoch's full set, and
\* drops the rest.
Receive ==
  /\ Len(chan) > 0
  /\ LET f == Head(chan) IN
     /\ chan' = Tail(chan)
     /\ IF watched /\ f.epoch = epoch /\ (f.kind = "full" \/ synced)
        THEN /\ synced' = TRUE /\ hHeld' = f.held /\ hFocus' = f.focus
             /\ pending' = pending + f.presses
             /\ accepted' = accepted + f.presses
             /\ recent' = TRUE /\ stale' = FALSE /\ from' = "owner"
             /\ delivered' = delivered
        ELSE UNCHANGED HostVars
  /\ UNCHANGED <<watched, epoch, up, connects>> /\ UNCHANGED ClientVars
  /\ UNCHANGED PlayerVars

\* Another peer sends a frame for this player. The feeder refuses its stream.
ReceiveOther ==
  /\ watched /\ synced /\ PeerCarriesGame("other")
  /\ hHeld' = TRUE /\ from' = "other"
  /\ UNCHANGED <<synced, hFocus, recent, stale, pending, delivered, accepted>>
  /\ UNCHANGED <<watched, epoch, up, connects, chan>> /\ UNCHANGED ClientVars
  /\ UNCHANGED PlayerVars

\* The host's deadline timer: a frame since the last firing rearms it, and
\* none marks the stream stale. The copy stays, so a short gap does not cut
\* the throttle.
Deadline ==
  /\ watched /\ synced /\ ~stale
  /\ IF recent THEN recent' = FALSE /\ stale' = stale
     ELSE stale' = TRUE /\ recent' = recent
  /\ UNCHANGED <<synced, hHeld, hFocus, pending, delivered, accepted, from>>
  /\ UNCHANGED <<watched, epoch, up, connects, chan>> /\ UNCHANGED ClientVars
  /\ UNCHANGED PlayerVars

\* A server frame gives the mod the presses held since the last one.
ModFrame ==
  /\ pending > 0
  /\ delivered' = delivered + pending /\ pending' = 0
  /\ UNCHANGED <<synced, hHeld, hFocus, recent, stale, accepted, from>>
  /\ UNCHANGED <<watched, epoch, up, connects, chan>> /\ UNCHANGED ClientVars
  /\ UNCHANGED PlayerVars

Next ==
  \/ Watch \/ Unwatch \/ Connect \/ Lose \/ DeliverWant
  \/ SendFull \/ SendDelta \/ Close
  \/ Press \/ Release \/ Refocus
  \/ Receive \/ ReceiveOther \/ Deadline \/ ModFrame

Spec == Init /\ [][Next]_vars
        /\ WF_vars(DeliverWant) /\ WF_vars(SendFull) /\ WF_vars(SendDelta)
        /\ WF_vars(Close) /\ WF_vars(Receive) /\ WF_vars(ModFrame)

\* Constraint 2: no other peer's frame reaches the mod.
OwnerOnly == from # "other"

\* An unfocused stream shows nothing held.
UnfocusedIdle == State = "unfocused" => ~hHeld

\* Every press the host applied reaches the mod or waits for its next frame,
\* however many frames arrived between two server frames.
PressesKept == delivered + pending = accepted

\* The devices are open only over a live link.
OpenOnlyLinked == open => up

\* Constraint 3: once no mod watches, the client closes the devices.
ClosesWithoutWatch == (~watched) ~> (~open \/ watched)

\* Constraint 7: a watched player with a link reaches a live or unfocused
\* stream again, whatever watches ended and began before.
Recovers == (watched /\ up) ~> (State \in {"live", "unfocused"} \/ ~watched \/ ~up)
====
