// Heroes lists every released hero alphabetically by the name players read,
// with the game's name, fixed id and small portrait for each, and matches the
// names players type. The table follows the game's hero data; a hero the game
// adds or releases joins it in a Modlock release.

/** Titled is something players pick by name: the game's name and the one players read. */
export interface Titled {
  readonly name: string
  readonly title: string
}

/** Hero is one released hero: its game name, such as 'hero_atlas', its title and its fixed id. */
export interface Hero extends Titled {
  readonly id: number
  /** portrait is the hero's small stock portrait, as an image's src. */
  readonly portrait: string
}

/** heroes are the released heroes, by title. */
export const heroes: readonly Hero[] = [
  {
    name: 'hero_atlas',
    id: 6,
    title: 'Abrams',
    portrait: 'file://{images}/heroes/bull_sm.psd',
  },
  {
    name: 'hero_fencer',
    id: 77,
    title: 'Apollo',
    portrait: 'file://{images}/heroes/fencer_sm.psd',
  },
  {
    name: 'hero_baba',
    id: 88,
    title: 'Baba',
    portrait: 'file://{images}/heroes/baba_sm.psd',
  },
  {
    name: 'hero_bebop',
    id: 15,
    title: 'Bebop',
    portrait: 'file://{images}/heroes/bebop_sm.psd',
  },
  {
    name: 'hero_punkgoat',
    id: 72,
    title: 'Billy',
    portrait: 'file://{images}/heroes/punkgoat_sm.psd',
  },
  {
    name: 'hero_nano',
    id: 16,
    title: 'Calico',
    portrait: 'file://{images}/heroes/nano_sm.psd',
  },
  {
    name: 'hero_unicorn',
    id: 81,
    title: 'Celeste',
    portrait: 'file://{images}/heroes/unicorn_sm.psd',
  },
  {
    name: 'hero_deadpack',
    id: 78,
    title: 'Deadman Danny',
    portrait: 'file://{images}/heroes/deadpack_sm.psd',
  },
  {
    name: 'hero_drifter',
    id: 64,
    title: 'Drifter',
    portrait: 'file://{images}/heroes/drifter_sm.psd',
  },
  {
    name: 'hero_dynamo',
    id: 11,
    title: 'Dynamo',
    portrait: 'file://{images}/heroes/sumo_sm.psd',
  },
  {
    name: 'hero_necro',
    id: 76,
    title: 'Graves',
    portrait: 'file://{images}/heroes/necro_sm.psd',
  },
  {
    name: 'hero_orion',
    id: 17,
    title: 'Grey Talon',
    portrait: 'file://{images}/heroes/archer_sm.psd',
  },
  {
    name: 'hero_haze',
    id: 13,
    title: 'Haze',
    portrait: 'file://{images}/heroes/haze_sm.psd',
  },
  {
    name: 'hero_astro',
    id: 14,
    title: 'Holliday',
    portrait: 'file://{images}/heroes/astro_sm.psd',
  },
  {
    name: 'hero_inferno',
    id: 1,
    title: 'Infernus',
    portrait: 'file://{images}/heroes/inferno_sm.psd',
  },
  {
    name: 'hero_tengu',
    id: 20,
    title: 'Ivy',
    portrait: 'file://{images}/heroes/tengu_sm.psd',
  },
  {
    name: 'hero_kelvin',
    id: 12,
    title: 'Kelvin',
    portrait: 'file://{images}/heroes/kelvin_sm.psd',
  },
  {
    name: 'hero_ghost',
    id: 4,
    title: 'Lady Geist',
    portrait: 'file://{images}/heroes/spectre_sm.psd',
  },
  {
    name: 'hero_lash',
    id: 31,
    title: 'Lash',
    portrait: 'file://{images}/heroes/lash_sm.psd',
  },
  {
    name: 'hero_forge',
    id: 8,
    title: 'McGinnis',
    portrait: 'file://{images}/heroes/engineer_sm.psd',
  },
  {
    name: 'hero_vampirebat',
    id: 63,
    title: 'Mina',
    portrait: 'file://{images}/heroes/vampirebat_sm.psd',
  },
  {
    name: 'hero_mirage',
    id: 52,
    title: 'Mirage',
    portrait: 'file://{images}/heroes/mirage_sm.psd',
  },
  {
    name: 'hero_krill',
    id: 18,
    title: 'Mo & Krill',
    portrait: 'file://{images}/heroes/digger_sm.psd',
  },
  {
    name: 'hero_nurse',
    id: 87,
    title: 'Nurse Harrow',
    portrait: 'file://{images}/heroes/nurse_sm.psd',
  },
  {
    name: 'hero_bookworm',
    id: 67,
    title: 'Paige',
    portrait: 'file://{images}/heroes/bookworm_sm.psd',
  },
  {
    name: 'hero_chrono',
    id: 10,
    title: 'Paradox',
    portrait: 'file://{images}/heroes/chrono_sm.psd',
  },
  {
    name: 'hero_synth',
    id: 50,
    title: 'Pocket',
    portrait: 'file://{images}/heroes/synth_sm.psd',
  },
  {
    name: 'hero_ratking',
    id: 84,
    title: 'Rat King',
    portrait: 'file://{images}/heroes/ratking_sm.psd',
  },
  {
    name: 'hero_familiar',
    id: 79,
    title: 'Rem',
    portrait: 'file://{images}/heroes/familiar_sm.psd',
  },
  {
    name: 'hero_gigawatt',
    id: 2,
    title: 'Seven',
    portrait: 'file://{images}/heroes/gigawatt_sm.psd',
  },
  {
    name: 'hero_shiv',
    id: 19,
    title: 'Shiv',
    portrait: 'file://{images}/heroes/shiv_sm.psd',
  },
  {
    name: 'hero_werewolf',
    id: 80,
    title: 'Silver',
    portrait: 'file://{images}/heroes/werewolf_sm.psd',
  },
  {
    name: 'hero_magician',
    id: 60,
    title: 'Sinclair',
    portrait: 'file://{images}/heroes/magician_sm.psd',
  },
  {
    name: 'hero_chessmaster',
    id: 85,
    title: 'Solomon',
    portrait: 'file://{images}/heroes/chessmaster_sm.psd',
  },
  {
    name: 'hero_doorman',
    id: 69,
    title: 'The Doorman',
    portrait: 'file://{images}/heroes/doorman_sm.psd',
  },
  {
    name: 'hero_priest',
    id: 65,
    title: 'Venator',
    portrait: 'file://{images}/heroes/priest_sm.psd',
  },
  {
    name: 'hero_frank',
    id: 66,
    title: 'Victor',
    portrait: 'file://{images}/heroes/frank_sm.psd',
  },
  {
    name: 'hero_hornet',
    id: 3,
    title: 'Vindicta',
    portrait: 'file://{images}/heroes/hornet_sm.png',
  },
  {
    name: 'hero_artist',
    id: 86,
    title: 'Violet',
    portrait: 'file://{images}/heroes/artist_sm.psd',
  },
  {
    name: 'hero_viscous',
    id: 35,
    title: 'Viscous',
    portrait: 'file://{images}/heroes/viscous_sm.psd',
  },
  {
    name: 'hero_viper',
    id: 58,
    title: 'Vyper',
    portrait: 'file://{images}/heroes/kali_sm.psd',
  },
  {
    name: 'hero_warden',
    id: 25,
    title: 'Warden',
    portrait: 'file://{images}/heroes/warden_sm.psd',
  },
  {
    name: 'hero_wraith',
    id: 7,
    title: 'Wraith',
    portrait: 'file://{images}/heroes/wraith_sm.psd',
  },
  {
    name: 'hero_yamato',
    id: 27,
    title: 'Yamato',
    portrait: 'file://{images}/heroes/yamato_sm.psd',
  },
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
