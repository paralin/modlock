// prop-stats keeps a prop hunt player's stats across sessions in the stats
// record mod.json declares under persist. It reads each player's record when
// they join and logs it once it arrives; /win ends a round the player won,
// with how many seconds they stayed hidden, counting the win, keeping the
// longest hide and unlocking the cone prop at three wins.
import {
  command,
  log,
  onFrame,
  onPlayerJoin,
  persist,
  type PersistRecord,
  type Player,
  toast,
} from 'modlock'

/** waiting holds the players whose record the mod has asked for and not logged. */
const waiting = new Set<Player>()

/** round numbers the rounds this session ends, so a resent update counts once. */
let round = 0

/** describe writes a record's stats as one line. */
function describe(record: PersistRecord): string {
  const unlocks = (record.unlocks as readonly string[] | undefined) ?? []
  return `roundsWon=${String(record.roundsWon ?? 0)} longestHide=${String(record.longestHide ?? 0)} unlocks=[${unlocks.join(',')}]`
}

// Read the record on join; it arrives a frame or more later.
onPlayerJoin(({ connection }) => {
  persist.get(connection.player, 'stats')
  waiting.add(connection.player)
})

// Log each record once it arrives.
onFrame(() => {
  for (const player of waiting) {
    const read = persist.get(player, 'stats')
    if (read.state === 'loading') continue
    waiting.delete(player)
    if (read.state === 'unavailable') {
      log(`player ${player.slot} stats unavailable: ${read.error}`)
      continue
    }
    log(`player ${player.slot} stats read: ${describe(read.record)}`)
  }
})

// End a round the player won, after hiding for the given seconds.
command('win', (player, args) => {
  const hidden = Number(args) || 0
  round++
  const key = `${Date.now()}-${round}`
  persist.update(
    player,
    'stats',
    { roundsWon: { add: 1 }, longestHide: { max: hidden } },
    { key },
    (record, error) => {
      if (!record) {
        toast(player, `Your win was not saved: ${error}`)
        return
      }
      log(`player ${player.slot} stats written: ${describe(record)}`)
      if (Number(record.roundsWon ?? 0) >= 3) {
        persist.update(
          player,
          'stats',
          { unlocks: { insert: 'cone' } },
          {
            key: `${key}-cone`,
          },
        )
      }
    },
  )
})
