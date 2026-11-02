// Rotation runs a lobby through a fight in turns: everyone waits in one line,
// each match seats whoever has played the fewest matches, the sides are split
// by a session rating so neither stacks, and the fighters go to the back of
// the line when it ends. Standings keeps each player's record by Steam ID for
// the session, so leaving and rejoining keeps their place in the order rather
// than resetting it. Every function here is pure: the mod keeps the line and
// calls these with what it holds, so a test runs them without the game.

/** startingRating is the rating a player starts the session at. */
export const startingRating = 1000

/** ratingStep is how far one even match moves each side's rating, by default. */
export const ratingStep = 32

/** Standing is one player's record for the session. */
export interface Standing {
  /** played counts the matches the player was seated for. */
  played: number
  rating: number
  wins: number
  losses: number
  /** streak counts the matches won in a row, zero after a loss. */
  streak: number
}

/**
 * Standings keeps a Standing for each Steam ID that joined this session. A
 * mod that keeps more per player, such as matches played as each hero,
 * passes extra to add those fields to each new standing.
 */
export class Standings<Extra extends object = object> {
  private readonly bySteamId = new Map<bigint, Standing & Extra>()

  constructor(private readonly extra: () => Extra = () => ({}) as Extra) {}

  /**
   * join is steamId's standing, made at the starting rating on their first
   * join. Its matches played rise to the fewest anyone present has played,
   * so joining late or rejoining never puts a player ahead of the line.
   */
  join(steamId: bigint, present: readonly Standing[]): Standing & Extra {
    // Make the standing on the first join, then raise it to the line.
    const least = fewestPlayed(present)
    let standing = this.bySteamId.get(steamId)
    if (!standing) {
      standing = {
        played: least,
        rating: startingRating,
        wins: 0,
        losses: 0,
        streak: 0,
        ...this.extra(),
      }
      this.bySteamId.set(steamId, standing)
    }
    standing.played = Math.max(standing.played, least)
    return standing
  }

  /** get is steamId's standing, or undefined before they join. */
  get(steamId: bigint): (Standing & Extra) | undefined {
    return this.bySteamId.get(steamId)
  }
}

/**
 * fewestPlayed is the fewest matches any of standings has played, zero with
 * none.
 */
export function fewestPlayed(standings: readonly Standing[]): number {
  let least = Infinity
  for (const standing of standings) {
    least = Math.min(least, standing.played)
  }
  return least === Infinity ? 0 : least
}

/**
 * nextMatch picks two sides of size players from the line: the 2 * size who
 * have played the fewest matches, the earlier in line first among equals,
 * dealt by rating in a snake (first, second, second, first, ...) so the sum
 * of ratings stays close. One a side is the two in rating order; two a side
 * is the best with the worst against the middle two. It returns undefined
 * while too few wait.
 */
export function nextMatch<T>(
  line: readonly T[],
  size: number,
  standing: (player: T) => Standing,
): [T[], T[]] | undefined {
  // The fewest played first; a stable sort keeps line order among equals.
  const seated = [...line]
    .sort((a, b) => standing(a).played - standing(b).played)
    .slice(0, 2 * size)
  if (size < 1 || seated.length < 2 * size) {
    return undefined
  }

  // Deal by rating, best first, turning at each end of the pair of sides.
  const ranked = seated.sort((a, b) => standing(b).rating - standing(a).rating)
  const sides: [T[], T[]] = [[], []]
  for (const [i, player] of ranked.entries()) {
    const round = Math.floor(i / 2)
    const first = i % 2 === 0 ? round % 2 === 0 : round % 2 === 1
    sides[first ? 0 : 1].push(player)
  }
  return sides
}

/**
 * afterMatch is the line once a match ends: everyone who did not fight
 * first, then the fighters, each part in line order.
 */
export function afterMatch<T>(line: readonly T[], fought: readonly T[]): T[] {
  const fighters = new Set(fought)
  return [
    ...line.filter((player) => !fighters.has(player)),
    ...line.filter((player) => fighters.has(player)),
  ]
}

/**
 * ratingChange is how far each winner's rating rises, and each loser's
 * falls, when a side rated winners beats a side rated losers: the Elo step,
 * larger for an upset.
 */
export function ratingChange(
  winners: number,
  losers: number,
  step = ratingStep,
): number {
  const expected = 1 / (1 + 10 ** ((losers - winners) / 400))
  return Math.round(step * (1 - expected))
}

/**
 * settle records a finished match: each side's ratings move by the change
 * between the sides' average ratings, and the wins, losses and streaks
 * count it. It returns the rating change, zero with an empty side.
 */
export function settle(
  winners: readonly Standing[],
  losers: readonly Standing[],
  step = ratingStep,
): number {
  const change =
    winners.length > 0 && losers.length > 0
      ? ratingChange(average(winners), average(losers), step)
      : 0
  for (const standing of winners) {
    standing.rating += change
    standing.wins++
    standing.streak++
  }
  for (const standing of losers) {
    standing.rating -= change
    standing.losses++
    standing.streak = 0
  }
  return change
}

/** average is the mean rating of standings. */
function average(standings: readonly Standing[]): number {
  let sum = 0
  for (const standing of standings) {
    sum += standing.rating
  }
  return sum / standings.length
}

/**
 * leastUsed is the option to give a player next so each gets an even share:
 * the index of the smallest of counts, or prefer when it ties for smallest.
 * Counting each player's matches as each hero and seating leastUsed gives
 * everyone every hero about equally.
 */
export function leastUsed(counts: readonly number[], prefer = 0): number {
  const least = Math.min(...counts)
  if (counts[prefer] === least) {
    return prefer
  }
  return counts.indexOf(least)
}

/**
 * leaderboard is the rows a viewer sees: players by wins and then rating,
 * the first rows of them, with viewer kept on the last row when they would
 * fall off the end.
 */
export function leaderboard<T>(
  players: readonly T[],
  standing: (player: T) => Standing,
  rows: number,
  viewer?: T,
): T[] {
  const ranked = [...players].sort(
    (a, b) =>
      standing(b).wins - standing(a).wins ||
      standing(b).rating - standing(a).rating,
  )
  const shown = ranked.slice(0, rows)
  if (
    viewer !== undefined &&
    shown.length > 0 &&
    players.includes(viewer) &&
    !shown.includes(viewer)
  ) {
    shown[shown.length - 1] = viewer
  }
  return shown
}
