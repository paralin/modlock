// inventory draws the bag the mod sends as the layout's data and turns a drag
// from one slot onto another into the console command
// "inventory_move <from> <to>". The mod answers with the new bag, so the
// slots change only when the mod agrees to the move.
'use strict'

const root = $.GetContextPanel()
const size = 16

/** slots holds each slot's panel and its name label, in order. */
const slots = []
for (let i = 0; i < size; i++) {
  slots.push({
    panel: root.FindChildTraverse(`Slot${i}`),
    name: root.FindChildTraverse(`Name${i}`),
  })
}

/** bag is the last bag the mod sent: an item or null for each slot. */
let bag = []

/** dragged is the slot a drag started from, or -1 between drags. */
let dragged = -1

/** draw paints each slot with the item the mod put there; an empty slot
 * takes the sheet's color. */
function draw(value) {
  bag = Array.isArray(value) ? value : []
  slots.forEach(({ panel, name }, i) => {
    const item = bag[i]
    name.text = item ? item.name : ''
    panel.style.backgroundColor = item ? item.color : null
  })
}

slots.forEach(({ panel }, i) => {
  // A drag starts only from a slot that holds an item, and carries a copy of
  // its name under the cursor.
  $.RegisterEventHandler('DragStart', panel, (_id, drag) => {
    const item = bag[i]
    if (!item) return false
    dragged = i
    const ghost = $.CreatePanel('Label', root, '')
    ghost.text = item.name
    ghost.hittest = false
    ghost.AddClass('ghost')
    ghost.style.backgroundColor = item.color
    drag.displayPanel = ghost
    drag.offsetX = 0
    drag.offsetY = 0
    drag.removePositionBeforeDrop = false
    return true
  })
  $.RegisterEventHandler('DragEnter', panel, () => {
    panel.AddClass('slot-target')
    return true
  })
  $.RegisterEventHandler('DragLeave', panel, () => {
    panel.RemoveClass('slot-target')
    return true
  })
  $.RegisterEventHandler('DragDrop', panel, () => {
    panel.RemoveClass('slot-target')
    if (dragged >= 0 && dragged !== i) {
      $.DispatchEvent('CitadelConCommand', `inventory_move ${dragged} ${i}`)
    }
    return true
  })
  $.RegisterEventHandler('DragEnd', panel, (_id, ghost) => {
    ghost.DeleteAsync(0)
    dragged = -1
    return true
  })
})

Modlock.data(draw)
