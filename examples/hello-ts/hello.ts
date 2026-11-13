// The actions a player can take in hello-ts. The chat commands in main.ts and
// the buttons in menu.tsx both call them.
import {
  createText,
  type Player,
  players,
  settingOn,
  type Vector,
  type WorldObject,
} from 'modlock'

/** greetings maps each choice of the greeting setting to its words. */
export const greetings: Record<string, string> = {
  hello: 'Hello',
  howdy: 'Howdy',
  gday: "G'day",
}

/** Sign is one player's floating text and where it rests. */
interface Sign {
  text: WorldObject
  rest: Vector
}

/**
 * signs holds each player's sign by their slot. When a map loads, the game
 * removes every sign, so forgetSigns empties this map.
 */
const signs = new Map<number, Sign>()

/**
 * nameOf returns the player's name, or "player" and their slot number when
 * they have left the server.
 */
export function nameOf(player: Player): string {
  const connection = players().find((c) => c.player.slot === player.slot)
  return connection?.name ?? `player ${player.slot}`
}

/**
 * greet says hello to player in the words their greeting setting chooses, and
 * counts the greeting under that choice.
 */
export function greet(player: Player, name: string): void {
  const choice = player.setting('greeting') ?? 'hello'
  player.chat(`${greetings[choice] ?? greetings.hello}, ${name}!`)
  player.addMetric('hello.said', 1, choice)
}

/**
 * placeSign floats words over player's hero and removes their earlier sign.
 * It returns false when the player has no living hero or the game could not
 * create the text.
 */
export function placeSign(player: Player, words: string): boolean {
  // A player whose hero is missing or dead gets no sign.
  const pawn = player.pawn()
  if (pawn === undefined || pawn.health <= 0) {
    return false
  }

  // Put the words 120 units above the hero's position, turned toward each
  // player who looks at them.
  const rest = { ...pawn.position, z: pawn.position.z + 120 }
  const text = createText({
    text: words,
    position: rest,
    fontSize: 80,
    color: 0xffd34dff,
    faceCamera: true,
  })
  if (text === undefined) {
    return false
  }

  // Replace the player's earlier sign and count the new one.
  signs.get(player.slot)?.text.remove()
  signs.set(player.slot, { text, rest })
  player.addMetric('signs.placed', 1)
  return true
}

/**
 * forgetSigns clears the mod's signs map. It does not remove the signs from
 * the game, because the game already removed every sign when it loaded the new
 * map.
 */
export function forgetSigns(): void {
  signs.clear()
}

/**
 * bobSigns lifts each sign up to 12 units above or below where it rests,
 * following the game clock's seconds, unless its owner turned the bob setting
 * off. It removes the sign of each player who has left.
 */
export function bobSigns(seconds: number): void {
  for (const [slot, sign] of signs) {
    const owner = players().find((c) => c.player.slot === slot)
    if (owner === undefined) {
      sign.text.remove()
      signs.delete(slot)
      continue
    }
    if (settingOn(owner.player, 'bob') !== false) {
      const lift = Math.sin(seconds * 2) * 12
      sign.text.move({ ...sign.rest, z: sign.rest.z + lift })
    }
  }
}
