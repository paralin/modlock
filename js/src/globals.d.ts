// The globals the Modlock QuickJS runtime gives a mod, beyond the standard
// ECMAScript library.

/** Console writes lines to the server log under the mod's name. */
interface Console {
  /** log writes its arguments, separated by spaces, as one line. */
  log(...parts: unknown[]): void
  /** info writes as log does. */
  info(...parts: unknown[]): void
  /** warn writes as log does. */
  warn(...parts: unknown[]): void
  /** error writes as log does. */
  error(...parts: unknown[]): void
  /** debug writes as log does. */
  debug(...parts: unknown[]): void
}

/** console writes to the server log. */
declare var console: Console

/** TextEncoder encodes strings as UTF-8. */
declare class TextEncoder {
  /** encoding is always "utf-8". */
  readonly encoding: 'utf-8'
  /** encode returns text's UTF-8 bytes. */
  encode(text?: string): Uint8Array
}

/** TextDecoder decodes UTF-8 bytes. */
declare class TextDecoder {
  /** encoding is always "utf-8". */
  readonly encoding: 'utf-8'
  /** decode returns the text in bytes. */
  decode(bytes?: ArrayBuffer | ArrayBufferView): string
}
