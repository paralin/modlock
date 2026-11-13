// Puppet shows the playback primitives. The first /puppet adds a ghost bot
// that walks a square starting where the player stands. After that, /puppet
// pause, play, seek <tick> and rate <n> control its playback, /puppet restore
// sets the player's hero to 700 health, level 5 and 1000 souls where it
// stands, and /puppet hit has the puppet hurt the player for 50.
import {
  command,
  log,
  type Player,
  Puppet,
  replayDamage,
  restoreHero,
  type Track,
} from 'modlock'

/** puppet is the live puppet, absent until /puppet adds one. */
let puppet: Puppet | undefined

/**
 * square is a track from origin around a square and back to origin, reaching
 * one corner each second.
 */
function square(origin: { x: number; y: number; z: number }): Track {
  const side = 300
  const offsets = [
    [0, 0],
    [side, 0],
    [side, side],
    [0, side],
    [0, 0],
  ]
  return {
    stepTicks: 64,
    x: offsets.map(([dx]) => origin.x + dx),
    y: offsets.map(([, dy]) => origin.y + dy),
    z: offsets.map(() => origin.z),
    yaw: [0, 90, 180, 270, 0],
    health: [700, 700, 700, 700, 700],
  }
}

command('puppet', (player: Player, args: string) => {
  // Read the verb and the player's hero.
  const [verb, value] = args.split(' ')
  const pawn = player.pawn()
  if (!pawn) return

  // Without a puppet, any verb adds one beside the player and starts it.
  if (!puppet) {
    puppet = Puppet.spawn(
      { name: 'Puppet', team: pawn.team === 3 ? 3 : 2, hero: 'hero_wraith' },
      square(pawn.position),
      { ghost: true },
    )
    puppet?.play()
    return
  }

  // Each other verb controls the puppet's playback or the player's hero.
  switch (verb) {
    case 'pause':
      puppet.pause()
      break
    case 'play':
      puppet.play()
      break
    case 'seek':
      puppet.seek(Number(value) || 0)
      break
    case 'rate':
      puppet.rate = Number(value) || 1
      break
    case 'restore':
      restoreHero(
        player,
        {
          position: pawn.position,
          facing: pawn.eyeAngles,
          health: 700,
          maxHealth: 700,
          level: 5,
          souls: 1000,
        },
        (error) => log('restore', error ?? 'held'),
      )
      break
    case 'hit':
      replayDamage({ victim: player, attacker: puppet.player, amount: 50 })
      break
  }
})
