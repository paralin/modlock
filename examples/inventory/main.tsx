// inventory gives each player a bag of sixteen slots, drawn by the mod's own
// layout in content/layout/inventory.xml. A player types /inventory to open or
// close the bag and, holding Tab to free the cursor, drags an item onto another
// slot. The layout sends the move as the console command
// "inventory_move <from> <to>", and the mod sends the new slots back as the
// layout's data. Only a host that has reviewed the mod's content draws the
// layout and runs its script; Modlock's own renderer draws an empty panel.
import {
  command,
  type Connection,
  hide,
  type Player,
  players,
  show,
} from 'modlock'

/** Item is what one slot holds. */
interface Item {
  readonly name: string
  readonly color: string
}

/** Bag is a player's slots in order; null marks an empty slot. */
type Bag = (Item | null)[]

/** size is the number of slots in a bag, four rows of four. */
const size = 16

/** starting fills a new bag's first slots; the rest start empty. */
const starting: readonly Item[] = [
  { name: 'Sword', color: '#c8553d' },
  { name: 'Shield', color: '#4f6d7a' },
  { name: 'Potion', color: '#7cae7a' },
  { name: 'Map', color: '#d9b26f' },
  { name: 'Key', color: '#e8c547' },
]

/** bags holds each player's bag by Steam account, so a rejoin keeps it. */
const bags = new Map<bigint, Bag>()

/**
 * open holds the connection generation of each slot whose bag is open. The
 * host clears a player's interface when they leave, so a new connection in
 * the slot starts closed.
 */
const open = new Map<number, number>()

/** connection returns the human playing in player's slot. */
function connection(player: Player): Connection | undefined {
  return players().find((c) => c.player.slot === player.slot && !c.bot)
}

/** isOpen reports whether c's bag is open. */
function isOpen(c: Connection): boolean {
  return open.get(c.player.slot) === c.generation
}

/** bagOf returns the account's bag, creating it full of the starting items. */
function bagOf(steamId: bigint): Bag {
  let bag = bags.get(steamId)
  if (bag === undefined) {
    bag = Array.from({ length: size }, (_, i) => starting[i] ?? null)
    bags.set(steamId, bag)
  }
  return bag
}

/** draw shows player their bag in the middle of the screen. */
function draw(player: Player, bag: Bag): void {
  show(
    player,
    <layout
      src="inventory.xml"
      data={bag}
      style={{ horizontalAlign: 'center', verticalAlign: 'center' }}
    />,
  )
}

// /inventory opens the bag, or closes it when it is open.
command('inventory', (player) => {
  // Only a human in the slot has a bag.
  const c = connection(player)
  if (c === undefined) return

  // Close an open bag.
  if (isOpen(c)) {
    open.delete(player.slot)
    hide(player)
    return
  }

  // Open it, filled with the starting items on first use.
  open.set(player.slot, c.generation)
  draw(player, bagOf(c.steamId))
})

// A move swaps the two slots, so dropping onto a full slot trades places. The
// mod checks both slots, since any player's game can send any command.
command('inventory_move', (player, args) => {
  // Only a player whose bag is open moves items.
  const c = connection(player)
  if (c === undefined || !isOpen(c)) return

  // Read two slots in the bag.
  const [from, to] = args.split(' ').map(Number)
  const valid = (slot: number) =>
    Number.isInteger(slot) && slot >= 0 && slot < size
  if (!valid(from) || !valid(to) || from === to) return

  // Swap them and send the new bag.
  const bag = bagOf(c.steamId)
  ;[bag[from], bag[to]] = [bag[to], bag[from]]
  draw(player, bag)
})
