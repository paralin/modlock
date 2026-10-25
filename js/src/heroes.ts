// Heroes lists every released hero alphabetically by the name players read,
// with the game's name and fixed id for each, and matches the names players
// type. The table follows the game's hero data; a hero the game adds or
// releases joins it in a Modlock release.

/** Titled is something players pick by name: the game's name and the one players read. */
export interface Titled {
  readonly name: string
  readonly title: string
}

/** Hero is one released hero: its game name, such as 'hero_atlas', its title and its fixed id. */
export interface Hero extends Titled {
  readonly id: number
}

/** heroes are the released heroes, by title. */
export const heroes: readonly Hero[] = [
  { name: 'hero_atlas', id: 6, title: 'Abrams' },
  { name: 'hero_fencer', id: 77, title: 'Apollo' },
  { name: 'hero_baba', id: 88, title: 'Baba' },
  { name: 'hero_bebop', id: 15, title: 'Bebop' },
  { name: 'hero_punkgoat', id: 72, title: 'Billy' },
  { name: 'hero_nano', id: 16, title: 'Calico' },
  { name: 'hero_unicorn', id: 81, title: 'Celeste' },
  { name: 'hero_deadpack', id: 78, title: 'Deadman Danny' },
  { name: 'hero_drifter', id: 64, title: 'Drifter' },
  { name: 'hero_dynamo', id: 11, title: 'Dynamo' },
  { name: 'hero_necro', id: 76, title: 'Graves' },
  { name: 'hero_orion', id: 17, title: 'Grey Talon' },
  { name: 'hero_haze', id: 13, title: 'Haze' },
  { name: 'hero_astro', id: 14, title: 'Holliday' },
  { name: 'hero_inferno', id: 1, title: 'Infernus' },
  { name: 'hero_tengu', id: 20, title: 'Ivy' },
  { name: 'hero_kelvin', id: 12, title: 'Kelvin' },
  { name: 'hero_ghost', id: 4, title: 'Lady Geist' },
  { name: 'hero_lash', id: 31, title: 'Lash' },
  { name: 'hero_forge', id: 8, title: 'McGinnis' },
  { name: 'hero_vampirebat', id: 63, title: 'Mina' },
  { name: 'hero_mirage', id: 52, title: 'Mirage' },
  { name: 'hero_krill', id: 18, title: 'Mo & Krill' },
  { name: 'hero_nurse', id: 87, title: 'Nurse Harrow' },
  { name: 'hero_bookworm', id: 67, title: 'Paige' },
  { name: 'hero_chrono', id: 10, title: 'Paradox' },
  { name: 'hero_synth', id: 50, title: 'Pocket' },
  { name: 'hero_ratking', id: 84, title: 'Rat King' },
  { name: 'hero_familiar', id: 79, title: 'Rem' },
  { name: 'hero_gigawatt', id: 2, title: 'Seven' },
  { name: 'hero_shiv', id: 19, title: 'Shiv' },
  { name: 'hero_werewolf', id: 80, title: 'Silver' },
  { name: 'hero_magician', id: 60, title: 'Sinclair' },
  { name: 'hero_chessmaster', id: 85, title: 'Solomon' },
  { name: 'hero_doorman', id: 69, title: 'The Doorman' },
  { name: 'hero_priest', id: 65, title: 'Venator' },
  { name: 'hero_frank', id: 66, title: 'Victor' },
  { name: 'hero_hornet', id: 3, title: 'Vindicta' },
  { name: 'hero_artist', id: 86, title: 'Violet' },
  { name: 'hero_viscous', id: 35, title: 'Viscous' },
  { name: 'hero_viper', id: 58, title: 'Vyper' },
  { name: 'hero_warden', id: 25, title: 'Warden' },
  { name: 'hero_wraith', id: 7, title: 'Wraith' },
  { name: 'hero_yamato', id: 27, title: 'Yamato' },
]

/** findHero matches what a player typed to a hero, as findTitled does. */
export function findHero(typed: string): Hero | undefined {
  return findTitled(heroes, typed)
}

/**
 * findTitled matches what a player typed, such as 'geist', 'Lady Geist' or
 * 'hero_ghost', to an entry of list: an exact name first, then the first
 * title that starts with or contains it.
 */
export function findTitled<T extends Titled>(
  list: readonly T[],
  typed: string,
): T | undefined {
  // Reduce what was typed to letters, digits and underscores.
  const key = typed.toLowerCase().replace(/[^a-z0-9_]/g, '')
  if (key === '') {
    return undefined
  }

  // Compare it with each name and plain title.
  const plain = (entry: T) =>
    entry.title.toLowerCase().replace(/[^a-z0-9]/g, '')
  return (
    list.find((entry) => entry.name === key || plain(entry) === key) ??
    list.find((entry) => plain(entry).startsWith(key)) ??
    list.find((entry) => plain(entry).includes(key))
  )
}
