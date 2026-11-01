---- MODULE Slider ----
\* Slider input stays scoped to the enabled control the player currently sees.
\* Positions are integer hundredths here; renderer tests cover real ranges.
EXTENDS Integers, FiniteSets

People == {"player"}
Buttons == {"drag", "wheel"}
VARIABLES mode, value, scale, thumb, echo, queued, violation
vars == <<mode, value, scale, thumb, echo, queued, violation>>

Shown(p) == IF mode = "zoom" THEN Buttons
            ELSE IF mode = "plain" THEN {"drag"} ELSE {}
Allowed(p, a) == a \in Shown(p)
Hint(p) == IF mode = "hidden" THEN "The mod has hidden the slider"
           ELSE IF mode = "disabled" THEN "The mod has disabled this input" ELSE ""
S == INSTANCE Surface WITH Actions <- Buttons, Next <- Hint
EveryShownActionWorks == S!EveryShownActionWorks
NobodyIsStuck == S!NobodyIsStuck

\* The zoom window changes from 0..100 to 20..100. A native position of 60
\* reads as 60 before zoom and 68 after it; both keep the exploration finite.
Project(v, s) == IF s = "wide" THEN v
                 ELSE IF v < 20 THEN 0 ELSE ((v - 20) * 100) \div 80
Init == /\ mode = "hidden" /\ value = 0 /\ thumb = 0
        /\ scale = "wide" /\ echo = FALSE
        /\ queued = "none" /\ violation = FALSE

\* A redraw may hide, disable, enable or remove wheel support while an input waits.
DrawHidden ==
  /\ mode' = "hidden"
  /\ UNCHANGED <<value, scale, thumb, echo, queued, violation>>
DrawDisabled ==
  /\ mode' = "disabled"
  /\ UNCHANGED <<value, scale, thumb, echo, queued, violation>>
DrawPlain ==
  /\ mode' = "plain"
  /\ UNCHANGED <<value, scale, thumb, echo, queued, violation>>
DrawZoom ==
  /\ mode' = "zoom"
  /\ UNCHANGED <<value, scale, thumb, echo, queued, violation>>

Drag ==
  /\ "drag" \in Shown("player")
  /\ queued = "none" /\ queued' = "drag"
  /\ UNCHANGED <<mode, value, scale, thumb, echo, violation>>

Wheel ==
  /\ "wheel" \in Shown("player")
  /\ queued = "none" /\ queued' = "wheel"
  /\ UNCHANGED <<mode, value, scale, thumb, echo, violation>>

\* The host checks the latest control. A wheel event never seeks the slider.
Deliver ==
  /\ queued # "none"
  /\ value' = IF queued = "drag" /\ Allowed("player", "drag")
               THEN IF scale = "wide" THEN 60 ELSE 68 ELSE value
  /\ scale' = IF queued = "wheel" /\ Allowed("player", "wheel")
               THEN "narrow" ELSE scale
  \* A pending value echo cannot suppress reprojection onto a changed scale.
  /\ thumb' = IF scale' # scale THEN Project(value', scale')
               ELSE IF queued = "drag" /\ Allowed("player", "drag") THEN 60 ELSE thumb
  /\ echo' = IF queued = "drag" /\ Allowed("player", "drag") THEN TRUE
              ELSE IF queued = "wheel" /\ Allowed("player", "wheel") THEN FALSE ELSE echo
  /\ violation' = violation \/
       (queued = "wheel" /\ value' # value)
  /\ queued' = "none"
  /\ UNCHANGED mode

Next == DrawHidden \/ DrawDisabled \/ DrawPlain \/ DrawZoom \/ Drag \/ Wheel \/ Deliver
Spec == Init /\ [][Next]_vars
BoundedPosition == thumb \in 0..100
ProjectionMatchesRange == thumb = Project(value, scale)
WheelNeverSeeks == ~violation
====
