// The local server's routes. Every answer is read from the player's own
// install: messages in the protobuf binary encoding, buffers and mips as
// stored bytes.
import type { MessageType } from '@aptre/protobuf-es-lite/message'

// route returns the server path of one asset route for an archive path.
export const route = (name: string, archivePath: string) =>
  `/api/assets/${name}/${archivePath}`

// fetchOk fetches a path and fails with the server's message.
async function fetchOk(path: string, signal?: AbortSignal): Promise<Response> {
  const response = await fetch(path, { signal })
  if (!response.ok)
    throw new Error((await response.text()).trim() || response.statusText)
  return response
}

// fetchBytes fetches a path's bytes.
export async function fetchBytes(
  path: string,
  signal?: AbortSignal,
): Promise<Uint8Array> {
  return new Uint8Array(await (await fetchOk(path, signal)).arrayBuffer())
}

// fetchMessage fetches a path's message.
export async function fetchMessage<T extends object>(
  type: MessageType<T>,
  path: string,
  signal?: AbortSignal,
): Promise<T> {
  return type.fromBinary(await fetchBytes(path, signal))
}

// Kind is how the page shows a file.
export type Kind = 'map' | 'model' | 'texture' | 'material' | 'data'

// kindOf returns how the page shows a file, by its extension.
export function kindOf(name: string): Kind {
  // Match the extensions the page draws, and show the rest as data.
  if (name.endsWith('.vwrld_c')) return 'map'
  if (name.endsWith('.vmdl_c')) return 'model'
  if (name.endsWith('.vtex_c')) return 'texture'
  if (name.endsWith('.vmat_c')) return 'material'
  return 'data'
}
