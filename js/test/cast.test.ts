import { expect, test } from 'bun:test'

import {
  type Connection as ConnectionMessage,
  HeroResponse,
  PawnResponse,
  PlayerRequest,
  PlayersChangedEvent,
  SelectHeroRequest,
} from '../../src/wasm/players/players.pb.js'
import {
  Call,
  FrameEvent,
  Reply,
  ServerCommandRequest,
} from '../../src/wasm/wasm.pb.js'
import { bind, deliver } from '../src/host.js'
import { Cast, type Connection, onFrame } from '../src/index.js'

/** Hero is the stand-in game's hero for one slot. */
interface Hero {
  hero: number
  team: number
  health: number
}

/** heroes holds each slot's hero in the stand-in game. */
const heroes = new Map<number, Hero>()

/** asks counts the seats each slot asked for, so the first can be lost. */
const asks = new Map<number, number>()

/** sent lists the calls that change the game, as Method slot. */
const sent: string[] = []

// The stand-in game answers pawn reads from heroes, loses each player's first
// hero selection, and records every call that changes the game.
bind((bytes) => {
  const call = Call.fromBinary(bytes)
  const request = call.request ?? new Uint8Array()
  switch (call.method) {
    case 'Pawn': {
      const slot = PlayerRequest.fromBinary(request).player ?? 0
      const hero = heroes.get(slot)
      return Reply.toBinary({
        response: PawnResponse.toBinary({
          pawn: hero && { entity: 100 + slot, ...hero },
        }),
      })
    }
    case 'SelectHero': {
      const {
        player = 0,
        hero,
        team = 0,
      } = SelectHeroRequest.fromBinary(request)
      const id = hero?.case === 'heroId' ? hero.value : 6
      sent.push(`SelectHero ${player} ${id} ${team}`)
      asks.set(player, (asks.get(player) ?? 0) + 1)
      if (asks.get(player) === 1) return Reply.toBinary({})
      heroes.set(player, { hero: id, team, health: 600 })
      return Reply.toBinary({ response: HeroResponse.toBinary({ hero: id }) })
    }
    case 'Respawn': {
      const slot = PlayerRequest.fromBinary(request).player ?? 0
      sent.push(`Respawn ${slot}`)
      heroes.get(slot)!.health = 600
      return Reply.toBinary({})
    }
    case 'ServerCommand':
      sent.push(ServerCommandRequest.fromBinary(request).command ?? '')
      return Reply.toBinary({})
  }
  return Reply.toBinary({})
})

/** report delivers the host's player report. */
function report(players: ConnectionMessage[]): void {
  deliver(
    Call.toBinary({
      method: 'PlayersChanged',
      request: PlayersChangedEvent.toBinary({ players }),
    }),
  )
}

/** frame delivers a frame at seconds and returns the calls it made. */
function frame(seconds: number): string[] {
  sent.length = 0
  deliver(
    Call.toBinary({
      method: 'Frame',
      request: FrameEvent.toBinary({ timeSeconds: seconds }),
    }),
  )
  return sent.slice()
}

test('a cast keeps one member per human and gets them the hero they want', () => {
  // A mode keeps a member per human, on hero 7 for team 3, back 3 seconds
  // after a fall; it kicks bots.
  const events: string[] = []
  const cast = new Cast({
    join: (c: Connection) => {
      events.push(`join ${c.name}`)
      return { name: c.name, player: c.player }
    },
    leave: (member) => events.push(`leave ${member.name}`),
    bots: 'kick',
  })
  onFrame(({ timeSeconds }) => {
    cast.frame(timeSeconds)
    for (const member of cast.members()) {
      cast.want(member.player, { hero: 7, team: 3, respawn: 3 })
    }
  })

  // A human still joining is no member; a bot is kicked once.
  const ada = {
    player: 1,
    steamId: 11n,
    name: 'Ada',
    ready: false,
    generation: 1,
  }
  const bot = { player: 2, name: 'Bot', bot: true, ready: true, generation: 1 }
  report([ada, bot])
  expect(frame(1)).toEqual(['kick "Bot"'])
  expect(cast.size).toBe(0)
  report([ada, bot])
  expect(frame(1.1)).toEqual([])

  // Ada joins; the frame after makes her want known.
  report([{ ...ada, ready: true }, bot])
  expect(frame(2)).toEqual([])
  expect(events).toEqual(['join Ada'])

  // The first seat is lost, so Cast asks again a second later, not sooner.
  expect(frame(2.1)).toEqual(['SelectHero 1 7 3'])
  expect(frame(2.5)).toEqual([])
  expect(frame(3.1)).toEqual(['SelectHero 1 7 3'])

  // A seated hero settles: frames ask the game for nothing.
  expect(frame(3.2)).toEqual([])
  expect(frame(10)).toEqual([])
  const [member] = cast.members()
  expect(cast.pawn(member!.player)?.hero).toBe(7)

  // A fallen hero is asked back once its delay passes, once a second.
  heroes.get(1)!.health = 0
  expect(frame(11)).toEqual([])
  expect(frame(13.9)).toEqual([])
  expect(frame(14)).toEqual(['Respawn 1'])
  expect(frame(14.5)).toEqual([])

  // A reconnect is a leave and a join; a leave ends the member.
  events.length = 0
  report([{ ...ada, ready: true, generation: 2 }])
  frame(20)
  expect(events).toEqual(['leave Ada', 'join Ada'])
  report([])
  frame(21)
  expect(events).toEqual(['leave Ada', 'join Ada', 'leave Ada'])
  expect(cast.size).toBe(0)
})
