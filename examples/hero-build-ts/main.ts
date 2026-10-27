// hero-build-ts reads a hero's recommended build from the host's game data.
// Players type /build hero_inferno to see its categories and items.
import { command, heroBuild } from 'modlock'

command('build', (player, args) => {
  const hero = args === '' ? 'hero_inferno' : args
  const categories = heroBuild(hero)
  if (categories.length === 0) {
    player.chat(`No build for ${hero}.`)
    return
  }
  for (const category of categories) {
    player.chat(
      `${category.name}: ${category.items.map((i) => i.item).join(', ')}`,
    )
  }
})
