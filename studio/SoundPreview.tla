---- MODULE SoundPreview ----
EXTENDS Naturals, FiniteSets
\* Local archive playback is opt-in. Replacing the selection stops its audio.
People == {"creator"}
Buttons == {"select", "play", "pause", "seek", "volume", "download", "retry"}
VARIABLES selected, playing, phase
vars == <<selected, playing, phase>>
Shown(p) == {"select"} \cup
  CASE phase = "ready" -> {"play", "seek", "volume", "download"}
    [] phase = "playing" -> {"pause", "seek", "volume", "download"}
    [] phase = "failed" -> {"retry"}
    [] OTHER -> {}
Allowed(p, a) == a \in Shown(p)
Hint(p) == CASE phase = "loading" -> "Reading the selected sound"
            [] phase = "unsupported" -> "Choose another sound to preview"
            [] OTHER -> ""
S == INSTANCE Surface WITH Actions <- Buttons, Next <- Hint
EveryShownActionWorks == S!EveryShownActionWorks
NobodyIsStuck == S!NobodyIsStuck
Init == /\ selected = 0 /\ playing = 0 /\ phase = "idle"
Select == /\ selected' \in {1, 2} /\ playing' = 0 /\ phase' = "loading"
Loaded == /\ phase = "loading" /\ phase' \in {"ready", "unsupported", "failed"}
          /\ UNCHANGED <<selected, playing>>
Play == /\ phase = "ready" /\ playing' = selected /\ phase' = "playing"
        /\ UNCHANGED selected
Stop == /\ phase = "playing" /\ playing' = 0 /\ phase' = "ready"
        /\ UNCHANGED selected
Fail == /\ phase \in {"ready", "playing"} /\ phase' = "failed" /\ playing' = 0
        /\ UNCHANGED selected
Retry == /\ phase = "failed" /\ phase' = "loading" /\ UNCHANGED <<selected, playing>>
Next == Select \/ Loaded \/ Play \/ Stop \/ Fail \/ Retry
Spec == Init /\ [][Next]_vars
OnlySelectedPlays == playing = 0 \/ (playing = selected /\ phase = "playing")
SelectionIsSilent == phase = "loading" => playing = 0
====
