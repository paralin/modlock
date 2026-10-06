// The actions players take in hello-ts, shared by its commands and its menu.
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

/** signs holds each player's sign by slot; a new world removes them all. */
const signs = new Map<number, Sign>()

/** nameOf returns the player's name, or their slot when they have left. */
export function nameOf(player: Player): string {
  const connection = players().find((c) => c.player.slot === player.slot)
  return connection?.name ?? `player ${player.slot}`
}

/** greet says hello to player in the words their setting chooses. */
export function greet(player: Player, name: string): void {
  const choice = player.setting('greeting') ?? 'hello'
  player.chat(`${greetings[choice] ?? greetings.hello}, ${name}!`)
  player.addMetric('hello.said', 1, choice)
}

/**
 * placeSign raises text over player's hero, replacing their earlier sign.
 * It returns false when the player has no hero to stand over.
 */
export function placeSign(player: Player, words: string): boolean {
  // Only a living hero holds a sign.
  const pawn = player.pawn()
  if (pawn === undefined || pawn.health <= 0) {
    return false
  }

  // Float the words above the hero's head, turned toward each viewer.
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

/** forgetSigns drops every sign; a new world has already removed them. */
export function forgetSigns(): void {
  signs.clear()
}

/**
 * bobSigns moves each sign whose owner wants it up and down at seconds on
 * the game clock, and removes the signs of players who left.
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
