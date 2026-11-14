"""heroes lists every released hero alphabetically by the name players read,
with the game's name, fixed id and small portrait for each. The table follows
the game's hero data; a hero the game adds or releases joins it in a Modlock
release."""

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class Hero:
    """Hero is one released hero."""

    # name is the game's name for the hero, such as "hero_atlas".
    name: str
    # id is the hero's fixed id, as Pawn.hero reports it.
    id: int
    # title is the name players read, such as "Abrams".
    title: str
    # portrait is the hero's small stock portrait, as an image's src.
    portrait: str


# HEROES are the released heroes, by title.
HEROES: tuple[Hero, ...] = (
    Hero("hero_atlas", 6, "Abrams", "file://{images}/heroes/bull_sm.psd"),
    Hero("hero_fencer", 77, "Apollo", "file://{images}/heroes/fencer_sm.psd"),
    Hero("hero_baba", 88, "Baba", "file://{images}/heroes/baba_sm.psd"),
    Hero("hero_bebop", 15, "Bebop", "file://{images}/heroes/bebop_sm.psd"),
    Hero("hero_punkgoat", 72, "Billy", "file://{images}/heroes/punkgoat_sm.psd"),
    Hero("hero_nano", 16, "Calico", "file://{images}/heroes/nano_sm.psd"),
    Hero("hero_unicorn", 81, "Celeste", "file://{images}/heroes/unicorn_sm.psd"),
    Hero("hero_deadpack", 78, "Deadman Danny", "file://{images}/heroes/deadpack_sm.psd"),
    Hero("hero_drifter", 64, "Drifter", "file://{images}/heroes/drifter_sm.psd"),
    Hero("hero_dynamo", 11, "Dynamo", "file://{images}/heroes/sumo_sm.psd"),
    Hero("hero_necro", 76, "Graves", "file://{images}/heroes/necro_sm.psd"),
    Hero("hero_orion", 17, "Grey Talon", "file://{images}/heroes/archer_sm.psd"),
    Hero("hero_haze", 13, "Haze", "file://{images}/heroes/haze_sm.psd"),
    Hero("hero_astro", 14, "Holliday", "file://{images}/heroes/astro_sm.psd"),
    Hero("hero_inferno", 1, "Infernus", "file://{images}/heroes/inferno_sm.psd"),
    Hero("hero_tengu", 20, "Ivy", "file://{images}/heroes/tengu_sm.psd"),
    Hero("hero_kelvin", 12, "Kelvin", "file://{images}/heroes/kelvin_sm.psd"),
    Hero("hero_ghost", 4, "Lady Geist", "file://{images}/heroes/spectre_sm.psd"),
    Hero("hero_lash", 31, "Lash", "file://{images}/heroes/lash_sm.psd"),
    Hero("hero_forge", 8, "McGinnis", "file://{images}/heroes/engineer_sm.psd"),
    Hero("hero_vampirebat", 63, "Mina", "file://{images}/heroes/vampirebat_sm.psd"),
    Hero("hero_mirage", 52, "Mirage", "file://{images}/heroes/mirage_sm.psd"),
    Hero("hero_krill", 18, "Mo & Krill", "file://{images}/heroes/digger_sm.psd"),
    Hero("hero_nurse", 87, "Nurse Harrow", "file://{images}/heroes/nurse_sm.psd"),
    Hero("hero_bookworm", 67, "Paige", "file://{images}/heroes/bookworm_sm.psd"),
    Hero("hero_chrono", 10, "Paradox", "file://{images}/heroes/chrono_sm.psd"),
    Hero("hero_synth", 50, "Pocket", "file://{images}/heroes/synth_sm.psd"),
    Hero("hero_ratking", 84, "Rat King", "file://{images}/heroes/ratking_sm.psd"),
    Hero("hero_familiar", 79, "Rem", "file://{images}/heroes/familiar_sm.psd"),
    Hero("hero_gigawatt", 2, "Seven", "file://{images}/heroes/gigawatt_sm.psd"),
    Hero("hero_shiv", 19, "Shiv", "file://{images}/heroes/shiv_sm.psd"),
    Hero("hero_werewolf", 80, "Silver", "file://{images}/heroes/werewolf_sm.psd"),
    Hero("hero_magician", 60, "Sinclair", "file://{images}/heroes/magician_sm.psd"),
    Hero("hero_chessmaster", 85, "Solomon", "file://{images}/heroes/chessmaster_sm.psd"),
    Hero("hero_doorman", 69, "The Doorman", "file://{images}/heroes/doorman_sm.psd"),
    Hero("hero_priest", 65, "Venator", "file://{images}/heroes/priest_sm.psd"),
    Hero("hero_frank", 66, "Victor", "file://{images}/heroes/frank_sm.psd"),
    Hero("hero_hornet", 3, "Vindicta", "file://{images}/heroes/hornet_sm.png"),
    Hero("hero_artist", 86, "Violet", "file://{images}/heroes/artist_sm.psd"),
    Hero("hero_viscous", 35, "Viscous", "file://{images}/heroes/viscous_sm.psd"),
    Hero("hero_viper", 58, "Vyper", "file://{images}/heroes/kali_sm.psd"),
    Hero("hero_warden", 25, "Warden", "file://{images}/heroes/warden_sm.psd"),
    Hero("hero_wraith", 7, "Wraith", "file://{images}/heroes/wraith_sm.psd"),
    Hero("hero_yamato", 27, "Yamato", "file://{images}/heroes/yamato_sm.psd"),
)

def _plain(text: str, keep: str = "") -> str:
    """_plain reduces text to its lowercase ASCII letters and digits, and the
    characters in keep."""
    return "".join(c for c in text.lower() if c.isascii() and c.isalnum() or c in keep)


# _BY_NAME holds each released hero under every name hero_named accepts,
# reduced as _plain reduces them. Game names go in last, so a title that
# equals another hero's game name could never take it.
_BY_NAME = {
    **{_plain(hero.title): hero for hero in HEROES},
    **{hero.name.removeprefix("hero_"): hero for hero in HEROES},
    **{hero.name: hero for hero in HEROES},
}


def hero_named(name: str) -> Hero | None:
    """hero_named returns the released hero that name names exactly: its game
    name, such as "hero_nano", that name without "hero_", such as "nano", or
    its title in any case, with or without spaces and punctuation, such as
    "Calico" or "ladygeist". Unlike find_hero it matches no part of a name, so
    a misspelled name finds no hero rather than another one."""
    return _BY_NAME.get(_plain(name, "_"))


def find_hero(typed: str) -> Hero | None:
    """find_hero matches what a player typed, such as "geist", "Lady Geist" or
    "hero_ghost", to a hero: an exact game name or title first, then the first
    title that starts with or contains it."""
    # Reduce what was typed to letters, digits and underscores.
    key = _plain(typed, "_")
    if key == "":
        return None

    # Compare it with each name and plain title.
    for hero in HEROES:
        if hero.name == key or _plain(hero.title) == key:
            return hero
    for hero in HEROES:
        if _plain(hero.title).startswith(key):
            return hero
    for hero in HEROES:
        if key in _plain(hero.title):
            return hero
    return None
