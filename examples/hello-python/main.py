# hello-python is the smallest Modlock mod in Python: it answers /hello in
# chat and reports the first server frame.
import modlock


# Greet each player who types /hello.
@modlock.command("hello")
def hello(player: modlock.Player, args: str) -> None:
    player.chat("Hello from Python!")


# seen records whether the first frame has been reported.
seen = False


# Report the first server frame once.
@modlock.on_frame
def first_frame(frame: modlock.FrameEvent) -> None:
    global seen
    if seen:
        return
    seen = True
    modlock.log(f"first frame at tick {frame.tick}")
