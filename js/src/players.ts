// The host reports the occupied player slots before an event whenever they
// changed, so players answers from memory instead of a call each frame.

import {
  ModRole as ModRolePb,
  PlayersResponse,
} from '../../src/wasm/players/players.pb.js'
import { type Connection, type ModRole, Player } from './host.gen.js'
import { call } from './host.js'

/** roster is the player list the host last reported, if any. */
let roster: Connection[] | undefined = undefined

/** watchers hear each player list the host reports. */
const watchers: ((players: readonly Connection[]) => void)[] = []

/** keepPlayers keeps the player list the host reported and tells the watchers. */
export function keepPlayers(players: Connection[]): void {
  roster = players
  for (const watcher of watchers) {
    watcher(players)
  }
}

/**
 * watchPlayers calls watcher with each player list the host reports, and at
 * once with the last one when a report already arrived.
 */
export function watchPlayers(
  watcher: (players: readonly Connection[]) => void,
): void {
  watchers.push(watcher)
  if (roster !== undefined) watcher(roster)
}

/**
 * players lists the occupied player slots in slot order. It answers from the
 * roster the host last reported, and asks the host until a report arrives.
 */
export function players(): Connection[] {
  if (roster !== undefined) return roster.slice()
  const reply = call('Players', new Uint8Array())
  if (reply === undefined) return []
  return (PlayersResponse.fromBinary(reply).players ?? []).map((c) => ({
    player: new Player(c.player ?? 0),
    steamId: c.steamId ?? 0n,
    name: c.name ?? '',
    bot: c.bot ?? false,
    ready: c.ready ?? false,
    generation: c.generation ?? 0,
    roles: (c.roles ?? []).flatMap((role) => roleNames[role] ?? []),
  }))
}

/** roleNames names each role a player can hold. */
const roleNames: Partial<Record<ModRolePb, ModRole>> = {
  [ModRolePb.DEVELOPER]: 'developer',
  [ModRolePb.TESTER]: 'tester',
}
