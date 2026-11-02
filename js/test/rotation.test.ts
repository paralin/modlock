import { expect, test } from 'bun:test'

import {
  afterMatch,
  leaderboard,
  leastUsed,
  nextMatch,
  settle,
  Standings,
  startingRating,
} from '../src/rotation.js'

// A session runs through join, two seatings, a result and the leaderboard,
// the way a rotation mod calls these each match.
test('a rotation session seats evenly and keeps standings', () => {
  const standings = new Standings(() => ({ heroes: [0, 0] }))
  const ids = [1n, 2n, 3n, 4n, 5n]
  const standing = (id: bigint) => standings.get(id)!
  for (const id of ids.slice(0, 4)) {
    standings.join(id, [])
  }

  // Four even players: the snake deal puts the first and fourth together.
  standing(1n).rating = 1100
  standing(2n).rating = 1050
  standing(3n).rating = 950
  standing(4n).rating = 900
  let line = ids.slice(0, 4)
  const [a, b] = nextMatch(line, 2, standing)!
  expect(a).toEqual([1n, 4n])
  expect(b).toEqual([2n, 3n])

  // The winners rise by the upset step and the losers fall by it.
  const change = settle(a.map(standing), b.map(standing))
  expect(change).toBe(16)
  expect(standing(1n)).toMatchObject({ rating: 1116, wins: 1, streak: 1 })
  expect(standing(2n)).toMatchObject({ rating: 1034, losses: 1, streak: 0 })
  for (const id of [...a, ...b]) {
    standing(id).played++
  }

  // A late joiner starts level with the fewest played and keeps the extra.
  const late = standings.join(5n, line.map(standing))
  expect(late).toMatchObject({ played: 1, rating: startingRating })
  expect(late.heroes).toEqual([0, 0])

  // A rejoin keeps the record rather than resetting it.
  expect(standings.join(1n, line.map(standing)).wins).toBe(1)

  // The fighters go to the back; the next one-a-side match takes the front.
  line = afterMatch([...line, 5n], [1n, 2n])
  expect(line).toEqual([3n, 4n, 5n, 1n, 2n])
  standing(3n).played = 0
  expect(nextMatch(line, 1, standing)!.flat()).toEqual([3n, 4n])
  expect(nextMatch(line.slice(0, 3), 2, standing)).toBeUndefined()

  // The viewer below the cut keeps the last row.
  const rows = leaderboard(line, standing, 2, 2n)
  expect(rows).toEqual([1n, 2n])
  expect(leaderboard(line, standing, 2, 5n)).toEqual([1n, 5n])

  // An even hero share prefers the default, else the one played least.
  expect(leastUsed([1, 1], 1)).toBe(1)
  expect(leastUsed([2, 1], 0)).toBe(1)
})
