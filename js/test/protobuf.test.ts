import { expect, test } from 'bun:test'

import { createMessageType, type JsonValue } from '@aptre/protobuf-es-lite'

import { bind } from '../src/host.js'

bind(() => new Uint8Array())

/** Page holds one bytes field and repeated numbers, as a mod's own message might. */
interface Page {
  values?: number[]
  track?: Uint8Array
  rows?: Row[]
}

/** Row is a small submessage of Page. */
interface Row {
  id?: number
  name?: string
}

const Row = createMessageType<Row>('x.Row', () => [
  [1, 'id', 5],
  [2, 'name', 9],
])

const Page = createMessageType<Page>('x.Page', () => [
  [1, 'values', 17, 3],
  [3, 'track', 12],
  [4, 'rows', 11, 1, Row.codec],
])

test('fromJson reads a bytes field from base64', () => {
  const page = Page.fromJson({ track: 'AQID' })
  expect(page.track?.length).toBe(3)
  expect(page.track?.[2]).toBe(3)
  expect(Page.toJson(page)).toEqual({ track: 'AQID' })
})

/** Track holds only a bytes field. */
interface Track {
  track?: Uint8Array
}

const Track = createMessageType<Track>('x.Track', () => [[3, 'track', 12]])

test('fromJson reads a bytes field of parsed JSON', () => {
  const track = Track.fromJson(JSON.parse('{"track":"AQID"}') as JsonValue)
  expect(track.track?.length).toBe(3)
  expect(Track.fromJson({ track: '' }).track?.length ?? 0).toBe(0)
  // Padding and white space are bytes the decoder's table has no entry for.
  expect(Track.fromJson({ track: 'AQI=' }).track?.length).toBe(2)
  expect(Track.fromJson({ track: 'AQ==' }).track?.[0]).toBe(1)
  expect(Track.fromJson({ track: 'AQ ID' }).track?.length).toBe(3)
  expect(Track.toJsonString(track)).toBe('{"track":"AQID"}')
})

test('fromBinary reads what toBinary wrote', () => {
  const values: number[] = []
  for (let i = 0; i < 3840; i++) values.push((i % 7) - 3)
  const rows: Row[] = []
  for (let i = 0; i < 160; i++) rows.push({ id: i, name: `row ${i}` })
  const page = Page.fromBinary(
    Page.toBinary({ values, track: new Uint8Array([1, 2, 3]), rows }),
  )
  expect(page.values?.length).toBe(3840)
  expect(page.values?.[8]).toBe(-2)
  expect(page.rows?.length).toBe(160)
  expect(page.rows?.[159]?.name).toBe('row 159')
  expect(page.track?.[1]).toBe(2)
})
