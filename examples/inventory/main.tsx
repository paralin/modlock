// inventory gives each player a bag of sixteen slots. The mod draws the bag's
// frame and puts its own layout, content/layout/inventory.xml, inside it. A
// player types /inventory to open or close the bag, which shows their cursor
// while it is open, and drags an item onto another slot to move it. The layout
// sends the move as the console command "inventory_move <from> <to>", and the
// mod sends the new slots back as the layout's data. The style sheet
// content/styles/inventory.css styles the frame and the layout, and fades the
// bag in when it opens. Only a host that has reviewed the mod's content draws
// the layout, runs its script and applies the sheet. Modlock's own renderer
// draws the frame unstyled, around an empty panel where the layout would go.
import './content/styles/inventory.css'
import {
  after,
  Buttons,
  command,
  freeCursor,
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

/** Bag is a player's slots in order, with null in each empty slot. */
type Bag = (Item | null)[]

/** size is the number of slots in a bag, four rows of four. */
const size = 16

/** starting is the items in a new bag's first slots; the rest start empty. */
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
 * open is the connection generation of each slot whose bag is open. The
 * host clears a player's interface when they leave, so a new connection in
 * the slot starts closed.
 */
const open = new Map<number, number>()

/**
 * bagBlock is the buttons the mod blocks while a player's bag is open, so a
 * click on a slot does not fire the weapon.
 */
const bagBlock = Buttons.attack | Buttons.altFire | Buttons.melee

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

/**
 * draw shows player their bag in the middle of the screen. The style sheet
 * fades the bag in when its panel gains the Bag-open class, so opening the
 * bag draws it once without the class first.
 */
function draw(player: Player, bag: Bag, opened = true): void {
  show(
    player,
    <panel class={opened ? 'Bag Bag-open' : 'Bag'}>
      <label class="Bag-title">Bag</label>
      <label class="Bag-hint">Drag an item to move it</label>
      <layout src="inventory.xml" data={bag} />
    </panel>,
  )
}

// /inventory opens the bag, or closes it when it is open.
command('inventory', (player) => {
  // Only a human in the slot has a bag.
  const c = connection(player)
  if (c === undefined) return

  // Close an open bag; hiding it gives the mouse back to aiming.
  if (isOpen(c)) {
    open.delete(player.slot)
    player.blockInput(0n)
    hide(player)
    return
  }

  // Open it, filled with the starting items on first use, and fade it in on
  // the next frames unless it closed meanwhile.
  open.set(player.slot, c.generation)
  const bag = bagOf(c.steamId)
  draw(player, bag, false)
  after(0.05, () => {
    if (isOpen(c)) draw(player, bag)
  })

  // Free the cursor. A click still fires the weapon while the cursor is free,
  // so the bag withholds the weapon buttons itself. The player can put the
  // cursor away with the game's own keys, and closing the bag and opening it
  // again brings it back.
  freeCursor(player, true)
  player.blockInput(bagBlock)
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
