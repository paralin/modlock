# A rotation mode

A rotation mode runs a lobby through one fight at a time: a duel ring, a
king of the hill, a two against two pit. Everyone waits in one line, each
match seats whoever has fought the least, the sides are split by a session
rating so neither stacks, and the players not fighting watch from the stands.
This guide builds one from Modlock's pieces:

| Piece                                | Call                                        |
| ------------------------------------ | ------------------------------------------- |
| Who is here                          | `Cast`                                      |
| Each player's record for the session | `Standings`, `Standing`                     |
| Who fights next, and on which side   | `nextMatch`, `afterMatch`                   |
| The result                           | `settle`, `ratingChange`                    |
| An even share of heroes              | `leastUsed`                                 |
| The leaderboard                      | `leaderboard`, `Roster`, `rosterRows`       |
| Waiting in the stands                | `holdBystander`, `playCamera`, `curve.hold` |
| Heroes left behind by a swap         | `Strays`                                    |

Every rotation call is pure: the mod keeps the line and the standings, and the
calls read what they are given. A test can run a whole session without the
game, as `js/test/rotation.test.ts` does.

## The line and the standings

Keep the line as an array of your own players, in the order they arrived. A
`Standings` keeps one `Standing` per Steam ID for the session, so a player who
leaves and rejoins keeps their record and their place in the order rather than
starting fresh. `join` makes a standing at `startingRating` on the first join
and raises its matches played to the fewest anyone present has played, so
arriving late, or leaving to skip a turn, never puts a player ahead of the
line.

Pass `Standings` a function to add your own fields to each new standing, such
as matches played as each hero:

```ts
import { Cast, Standings } from 'modlock'

const standings = new Standings(() => ({ heroes: [0, 0] }))
let line: Entrant[] = []

const cast = new Cast({
  join: ({ player, name, steamId }) => {
    const standing = standings.join(
      steamId,
      line.map((each) => each.standing),
    )
    const entrant = { player, name, standing }
    line.push(entrant)
    return entrant
  },
  leave: (entrant) => {
    line = line.filter((each) => each !== entrant)
  },
})
```

## The next match

`nextMatch(line, size, standing)` picks two sides of `size` players: the
`2 * size` who have played the fewest matches, the earlier in line first among
equals, dealt by rating in a snake (first, second, second, first) so the sides'
ratings stay close. One a side pairs the two; two a side puts the best with the
worst against the middle two. It returns nothing while too few wait, which is
the state the HUD shows as waiting for players.

```ts
const sides = nextMatch(line, size, (each) => each.standing)
if (sides) {
  for (const fighter of sides.flat()) {
    fighter.standing.played++
  }
}
```

Count the match as played when it is seated, not when it ends, so a fighter
who leaves mid-match still goes to the back of the order.

## The result

`settle(winners, losers)` records a finished match: both sides' ratings move
by the Elo step between their average ratings, larger for an upset, and the
wins, losses and streaks count it. It returns the change, for a banner such as
`+16`. `ratingChange` is the same step alone, and both take a `step` other than
`ratingStep` (32) for a mode whose matches should count more or less.

Then `afterMatch(line, fought)` sends the fighters to the back of the line,
everyone else keeping their order ahead of them.

```ts
const change = settle(
  winners.map((each) => each.standing),
  losers.map((each) => each.standing),
)
line = afterMatch(line, [...winners, ...losers])
```

## An even share of heroes

When the mode chooses each fighter's hero from a short list, count the
matches each player has played as each, and seat `leastUsed(counts, prefer)`:
the one they have played least, or `prefer` when it ties. Everyone then plays
every hero about equally, and two fighters can still start on different heroes
by preferring different ones.

```ts
const choice = leastUsed(fighter.standing.heroes, side === 0 ? 0 : 1)
fighter.standing.heroes[choice]++
```

## The leaderboard

`leaderboard(players, standing, rows, viewer)` ranks players by wins and then
rating and returns the first `rows`, with the viewer kept on the last row when
they would fall off the end, so everyone can find themselves. Draw it with
`Roster`, which shows each row's hero, avatar and a line of activity:

```ts
const rows = leaderboard(line, (each) => each.standing, 8, viewer)
show(
  viewer.player,
  Roster({
    rows: rows.map((each) => ({
      name: each.name,
      self: each === viewer,
      hero: each.hero,
      steamId: each.steamId,
      activity: `${each.standing.wins}-${each.standing.losses} · ${each.standing.rating}`,
    })),
  }),
)
```

## The stands

A player waiting for their turn can keep a hero in the world rather than
leaving it: seated in the stands, on a hero the fight does not use, watching
the ring. `holdBystander(entity)` keeps the hero out of play: it cannot
move, shoot, cast or zoom. Both teams' abilities cannot target it, bullets and
melee pass over it, and displacement cannot pull it out of its seat, so it
stands beside the fight without touching it.
`holdBystander(entity, false)` gives it back before the player fights.

Teleport the hero to its seat facing the ring, then pin the camera on the
ring with `playCamera`. The `target` param is the point the camera turns
around, so a target at the ring's center and a long `distance` give a view
the player circles with the mouse. Each param snaps back when its curve ends,
so hold it for longer than any wait, and clear the context when the player
leaves the stands:

```ts
import {
  type Angles,
  curve,
  holdBystander,
  playCamera,
  type Player,
  type Vector,
} from 'modlock'

const context = 'arena.stands'

function seat(
  player: Player,
  pawn: number,
  position: Vector,
  facing: Angles,
): boolean {
  player.teleport(position, facing)
  holdBystander(pawn)
  player.clearCamera(context)
  const move = playCamera(player, {
    context,
    priority: 4,
    tracks: {
      target: curve.hold({ x: 0, y: 0, z: 60 }, 3600),
      distance: curve.hold(1400, 3600),
      verticalOffset: curve.hold(0, 3600),
      horizontalOffset: curve.hold(0, 3600),
    },
  })
  return move !== undefined
}

function unseat(player: Player, pawn: number): void {
  holdBystander(pawn, false)
  player.clearCamera(context)
}
```

`playCamera` returns nothing while the player has no hero, so mark the seat
taken only once `seat` returns true, and try again on the next frame until then.
A hero dies to falls and world damage like any other, so respawn a bystander
that dies and seat it again.

## Heroes left behind

Swapping a player's hero, or moving them to the spectators and back, can leave
the old hero in the world with nobody driving it, and the game revives it on
its own respawn timer. Keep one `Strays` and call `sweep(now)` each frame: once
a hero has belonged to no player for `strayGraceSeconds`, it is killed once
and has its respawn held off for good. A hero still counts as a player's while
its controller names it, so a player who is joining or spectating keeps
theirs. The sweep looks once a second, so it costs one entity listing and two
field reads per hero a second.

```ts
import { onFrame, Strays } from 'modlock'

const strays = new Strays()
onFrame(({ timeSeconds }) => strays.sweep(timeSeconds))
```

The sweep leaves a stray's abilities in place: killing the ability entities of
a live hero pawn has ended the server process.
