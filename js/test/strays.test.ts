import { expect, test } from 'bun:test'

import { HoldModifierStateRequest } from '../../src/wasm/abilities/abilities.pb.js'
import { KillOptions } from '../../src/wasm/combat/combat.pb.js'
import { EntitiesResponse } from '../../src/wasm/entities/entities.pb.js'
import {
  FieldResponse,
  ReadFieldRequest,
  WriteFieldRequest,
} from '../../src/wasm/fields/fields.pb.js'
import { Call, Reply } from '../../src/wasm/wasm.pb.js'
import { Player } from '../src/host.gen.js'
import { bind } from '../src/host.js'
import { keepPlayers } from '../src/players.js'
import { Strays } from '../src/strays.js'

// The transport supplies native fields; Strays owns the abandonment decision.
test('disconnected controllers do not retain abandoned heroes while joining heroes stay protected', () => {
  // Six controllers retain their hero handle across their connection states.
  const states = [0, 1, 2, 3, 4, undefined]
  const hits: number[] = []
  const held: HoldModifierStateRequest[] = []
  const respawns: number[] = []
  keepPlayers([])
  bind((bytes) => {
    const call = Call.fromBinary(bytes)
    let response: Uint8Array = new Uint8Array()
    switch (call.method) {
      case 'FindEntities':
        response = EntitiesResponse.toBinary({
          entities: states.map((_, i) => i + 1),
        })
        break
      case 'ReadField': {
        const r = ReadFieldRequest.fromBinary(call.request)
        const entity = r.entity ?? 0
        const value =
          r.field === 'm_hController'
            ? entity + 100
            : r.field === 'm_hHeroPawn'
              ? entity - 100
              : r.field === 'm_iConnected'
                ? states[entity - 101]
                : r.field === 'm_iHealth'
                  ? 100
                  : undefined
        response = FieldResponse.toBinary(
          value === undefined
            ? {}
            : { value: { value: { case: 'number', value } } },
        )
        break
      }
      case 'KillEntity':
        hits.push(KillOptions.fromBinary(call.request).victim ?? 0)
        break
      case 'HoldModifierState':
        held.push(HoldModifierStateRequest.fromBinary(call.request))
        break
      case 'WriteField':
        respawns.push(WriteFieldRequest.fromBinary(call.request).entity ?? 0)
        break
    }
    return Reply.toBinary({ response })
  })

  // Nothing is removed during the grace period, then only disconnected heroes.
  const strays = new Strays()
  strays.sweep(0)
  strays.sweep(2)
  expect(hits).toEqual([])
  strays.sweep(3)
  expect(hits).toEqual([4, 5])
  expect(respawns).toEqual([4, 5])
  expect(
    held.filter((r) => r.state === 'MODIFIER_STATE_MELEE_DISABLED'),
  ).toEqual([
    { entity: 4, state: 'MODIFIER_STATE_MELEE_DISABLED', active: false },
    { entity: 5, state: 'MODIFIER_STATE_MELEE_DISABLED', active: false },
  ])

  // A controller that reconnects during the grace period retains its hero.
  hits.length = 0
  states[3] = 2
  strays.sweep(4)
  expect(hits).toEqual([5])
})

test('an away seat keeps its hero until the seat ends', () => {
  // One disconnected controller, entity 101, whose slot 100 is away.
  const hits: number[] = []
  const away = {
    player: Player.of(100),
    steamId: 1n,
    name: 'away',
    bot: false,
    ready: false,
    generation: 1,
    away: true,
    roles: [],
  }
  keepPlayers([away])
  bind((bytes) => {
    const call = Call.fromBinary(bytes)
    let response: Uint8Array = new Uint8Array()
    if (call.method === 'FindEntities') {
      response = EntitiesResponse.toBinary({ entities: [1] })
    } else if (call.method === 'ReadField') {
      const field = ReadFieldRequest.fromBinary(call.request).field
      const value = {
        m_hController: 101,
        m_hHeroPawn: 1,
        m_iConnected: 4,
        m_iHealth: 100,
      }[field ?? '']
      if (value !== undefined) {
        response = FieldResponse.toBinary({
          value: { value: { case: 'number', value } },
        })
      }
    } else if (call.method === 'KillEntity') {
      hits.push(KillOptions.fromBinary(call.request).victim ?? 0)
    }
    return Reply.toBinary({ response })
  })

  // Held while away, a stray once the seat is gone.
  const strays = new Strays()
  strays.sweep(0)
  strays.sweep(5)
  expect(hits).toEqual([])
  keepPlayers([])
  strays.sweep(6)
  strays.sweep(9)
  expect(hits).toEqual([1])
})
