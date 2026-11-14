# Modlock

Write, play and publish [Deadlock] custom game modes from VS Code.

[Modlock] runs mods inside a Deadlock game server. You write a mod in
TypeScript, JavaScript, Luau, Python or Go, Modlock builds it, and the server
runs it in a sandbox. This extension puts the whole loop in the editor: make a
mod, play it, see what players see, and publish it.

[Deadlock]: https://store.steampowered.com/app/1422450/Deadlock/
[Modlock]: https://paralin.github.io/modlock/

## Make a mod

Open a folder that holds a mod's `mod.json`, or run **Modlock: New Mod** to
create one. The Modlock view in the activity bar shows the session and offers
each action.

## Play it

**Run Mod** builds the mod, starts a local server with it, launches Deadlock
and joins. Each save rebuilds and reloads the mod while you play. Build errors
show in Problems, and the mod's log in the Modlock output. When a frame runs
slow, the line it spent its time on gets a warning.

On a computer without Deadlock, **Run Mod** runs the mod in the sandbox
instead: the same host limits and events, with a stand-in player in place of
the game. Type a chat command, such as `/hello`, in the Player View and press
the buttons your mod shows to play as them.

**Host for Friends** runs the same project and opens a friends lobby on your
computer. Copy its invite from the sidebar; friends open the link to join.

## See what players see

The **Player View** shows each mod's interface, drawn as the game lays it out,
and the chat lines, center text and announcements mods show. Click a player in
the Modlock view to open it. The view's other rows show where the mods run and
each mod's state; right-click them for their actions.

## Publish it

**Publish Mod** runs `modlock publish`, which uploads the mod to
[hyperline.gg], where players find and install it.

[hyperline.gg]: https://hyperline.gg

## The command line

The extension manages the `modlock` command line for you. It downloads the one
from the Modlock release it came with and puts it on `PATH` in VS Code's
terminals, so `modlock` there is the same one. When a newer release is out,
the command line moves to it on its own; turn `modlock.autoUpdate` off to be
asked first. To run your own build instead, set `modlock.path` to it.
