# hero-build-python reads a hero's recommended build from the host's game
# data. Players type /build hero_inferno to see its categories and items.
import modlock


# Show the hero's build, one chat line per category.
@modlock.command("build")
def build(player: modlock.Player, args: str) -> None:
    hero = args or "hero_inferno"
    categories = modlock.hero_build(hero)
    if not categories:
        player.chat(f"No build for {hero}.")
        return
    for category in categories:
        items = ", ".join(item.item for item in category.items)
        player.chat(f"{category.name}: {items}")
