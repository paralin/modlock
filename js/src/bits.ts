// Names for the game's input and collision bits.

/** Buttons names input bits for blockInput, watchInput and Player.press. */
export const Buttons = {
  /** attack is the primary fire button. */
  attack: 1n,
  /** use is the interact key. */
  use: 1n << 5n,
  /** melee is the light melee button. */
  melee: 1n << 11n,
  /** reload is the reload key. */
  reload: 1n << 13n,
  /** ability1 to ability3 are the hero's first three ability keys. */
  ability1: 1n << 33n,
  ability2: 1n << 34n,
  ability3: 1n << 35n,
  /** parry is the melee parry button. */
  parry: 1n << 42n,
} as const

/** Layers names collision layers for trace, as bits. */
export const Layers = {
  /** solid is the world and solid objects. */
  solid: 1n << 0n,
  /** hitbox is the hit boxes of heroes and units. */
  hitbox: 1n << 1n,
  /** trigger is invisible volumes that react to touch. */
  trigger: 1n << 2n,
  /** playerClip is walls that only stop heroes. */
  playerClip: 1n << 4n,
  /** worldGeometry is the map's static geometry. */
  worldGeometry: 1n << 14n,
  /** player is player-controlled heroes. */
  player: 1n << 18n,
  /** npc is units that are not players. */
  npc: 1n << 19n,
  /** hero, trooper and building are those units' bodies. */
  hero: 1n << 37n,
  trooper: 1n << 38n,
  building: 1n << 40n,
} as const
