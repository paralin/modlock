// Items lists the shop's items, generated from the game's item data, and
// finds one by the class name a mod gives giveItem, such as
// 'upgrade_ability_power_shard'. An element given an item shows the shop's own
// card for it under the pointer: <image src={item.icon} item={item.name} />.

import { itemTable } from './items.gen.js'

export { itemId } from './item-id.js'

/** Slot is the shop tab an item sits on. */
export type Slot = 'weapon' | 'vitality' | 'spirit'

/** Item is one of the shop's items. */
export interface Item {
  /** name is the item's class name, which giveItem takes. */
  readonly name: string
  /** id is the game's id of the item. */
  readonly id: number
  /** slot is the shop tab the item sits on. */
  readonly slot: Slot
  /** tier is the item's tier, 1 to 4. */
  readonly tier: number
  /** active is true for an item the player casts. */
  readonly active: boolean
  /** imbue is true for an item that strengthens an ability it is put on. */
  readonly imbue: boolean
  /** icon is the item's shop icon, as an image's src. */
  readonly icon: string
}

/** items are the shop's items, by class name. */
export const items: readonly Item[] = itemTable

/** item returns the shop's item with class name, or undefined. */
export function item(name: string): Item | undefined {
  return byName.get(name)
}

/** byName indexes the items by class name. */
const byName = new Map(items.map((each) => [each.name, each]))
