import { expect, test } from 'bun:test'

import { BoardPage, ScoreReport } from '../../src/wasm/scores/scores.pb.js'
import {
  Call,
  LogRequest,
  Reply,
  ServiceCall,
  ServiceReply,
} from '../../src/wasm/wasm.pb.js'
import { Player } from '../src/host.gen.js'
import { bind } from '../src/host.js'
import { readBoard, reportScore } from '../src/scores.js'

/** answer is what the bound transport replies with next. */
let answer = Reply.toBinary({})

/** sent holds the Calls the transport carried, decoded. */
const sent: { method: string; request: Uint8Array }[] = []
bind((bytes) => {
  const decoded = Call.fromBinary(bytes)
  sent.push({
    method: decoded.method ?? '',
    request: decoded.request ?? new Uint8Array(),
  })
  // A log line succeeds whatever the service answered.
  return decoded.method === 'Log' ? Reply.toBinary({}) : answer
})

test('a score reaches the scores service, or is logged when no host keeps scores', () => {
  // A host with the service takes the report as encoded.
  sent.length = 0
  answer = Reply.toBinary({ response: ServiceReply.toBinary({}) })
  const player = Player.of(3)
  expect(
    reportScore(player, 'trial.time', 41250.4, {
      partition: { course: 'canyon' },
      key: 'run-7',
    }),
  ).toBe(true)
  const call = ServiceCall.fromBinary(sent[0]!.request)
  expect([call.service, call.method]).toEqual(['scores', 'Report'])
  const report = ScoreReport.fromBinary(call.payload)
  expect([report.player, report.value, report.partition, report.key]).toEqual([
    3,
    41250n,
    { course: 'canyon' },
    'run-7',
  ])

  // A host without it refuses, and the mod logs the score instead.
  sent.length = 0
  answer = Reply.toBinary({ error: 'this host provides no service scores' })
  expect(
    reportScore(player, 'trial.time', 39000, {
      partition: { course: 'canyon' },
    }),
  ).toBe(false)
  const logged = LogRequest.fromBinary(sent[1]!.request).message
  expect(logged).toBe(
    'score trial.time[course=canyon]: player 3 39000 (not reported: the host keeps no scores)',
  )
})

test('a board page decodes its rows and the asked player', () => {
  const page = BoardPage.toBinary({
    rows: [
      {
        rank: 1,
        steamId: 76561198000000001n,
        name: 'Mira',
        value: 39000n,
        count: 4,
      },
    ],
    player: {
      rank: 38,
      steamId: 76561198000000002n,
      name: 'Jun',
      value: 52000n,
      count: 1,
    },
  })
  answer = Reply.toBinary({
    response: ServiceReply.toBinary({ payload: page }),
  })
  const read = readBoard('trial.time', {
    partition: { course: 'canyon' },
    player: Player.of(1),
  })
  expect(read?.rows.map((r) => [r.rank, r.name, r.value])).toEqual([
    [1, 'Mira', 39000],
  ])
  expect([read?.player?.rank, read?.loading]).toEqual([38, false])

  answer = Reply.toBinary({ error: 'this host provides no service scores' })
  expect(readBoard('trial.time')).toBeUndefined()
})
