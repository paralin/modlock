// The modlock/kit module holds parts that game modes build from the modlock
// module's calls: a catalog that drops spots into the world, recorded heroes
// played back as puppets, race gates, match rotation and ratings, and sweeps
// for players and heroes left behind. Each part reaches the game only through
// modlock, so a mod that imports none of them pays nothing for them.
//
//   import { onStart } from 'modlock'
//   import { sweepStragglers } from 'modlock/kit'
//
//   onStart(() => sweepStragglers({ connectSeconds: 90 }))

export {
  Hook,
  type HookAbility,
  hookAbility,
  type HookPlayer,
  type HookRead,
  hookUpgrades,
  type ReadHandle,
} from './hook.js'
export {
  RaceGate,
  type RaceGateOptions,
  raceGateResources,
} from './racegate.js'
export {
  afterMatch,
  fewestPlayed,
  leaderboard,
  leastUsed,
  nextMatch,
  ratingChange,
  ratingStep,
  settle,
  type Standing,
  Standings,
  startingRating,
} from './rotation.js'
export {
  bystanderStates,
  holdBystander,
  parkedStates,
  parkHeight,
  spectateFrom,
} from './bystander.js'
export { Strays, strayGraceSeconds } from './strays.js'
export {
  Stragglers,
  stragglerConnectSeconds,
  type StragglersOptions,
  sweepStragglers,
} from './stragglers.js'
export {
  poseAt,
  type Pose,
  Puppet,
  type PuppetOptions,
  type PuppetState,
  ghostStates,
  ticksPerSecond,
  type Track,
  trackTicks,
} from './puppet.js'
export { type ReplayedHit, replayDamage } from './replayed-damage.js'
export { type CatalogEntry, Dropper, dropperButtons } from './dropper.js'
export { LoadedSpot, loadSpot, type Placed, triggers } from './loaded-spot.js'
