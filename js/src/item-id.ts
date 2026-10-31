// itemId names an item the way the game does: the string token of its class
// name, MurmurHash2 of the name as written with the token seed 0x31415926.

/** itemId returns the game's id of the item with class name, such as 'upgrade_ability_power_shard'. */
export function itemId(name: string): number {
  // Seed the hash with the name's length.
  const m = 0x5bd1e995
  const data = new TextEncoder().encode(name)
  const byte = (at: number) => data[at] ?? 0
  let h = (0x31415926 ^ data.length) >>> 0
  let i = 0

  // Mix in every whole four bytes.
  for (; data.length - i >= 4; i += 4) {
    let k =
      (byte(i) |
        (byte(i + 1) << 8) |
        (byte(i + 2) << 16) |
        (byte(i + 3) << 24)) >>>
      0
    k = Math.imul(k, m) >>> 0
    k = (k ^ (k >>> 24)) >>> 0
    k = Math.imul(k, m) >>> 0
    h = Math.imul(h, m) >>> 0
    h = (h ^ k) >>> 0
  }

  // Mix in the last one to three bytes, then finish.
  const rest = data.length - i
  if (rest >= 3) h = (h ^ (byte(i + 2) << 16)) >>> 0
  if (rest >= 2) h = (h ^ (byte(i + 1) << 8)) >>> 0
  if (rest >= 1) {
    h = (h ^ byte(i)) >>> 0
    h = Math.imul(h, m) >>> 0
  }
  h = (h ^ (h >>> 13)) >>> 0
  h = Math.imul(h, m) >>> 0
  return (h ^ (h >>> 15)) >>> 0
}
