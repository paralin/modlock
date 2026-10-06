// dropper builds a spot in play: /build opens the catalog, and every player
// drops into the same spot. /probe checks solid objects in front of the
// player and times placing many of them. Started with the argument probe, the
// mod seats a bot, runs the same checks, drops the bot onto a crate, and quits
// with PASS or FAIL.
import {
  addBot,
  type CatalogEntry,
  command,
  Dropper,
  dropperButtons,
  encodeSpot,
  type LoadedSpot,
  loadSpot,
  log,
  onFrame,
  onInput,
  onStart,
  type Player,
  players,
  precache,
  serverCommand,
  type SpotObject,
  trace,
  triggers,
  type Vector,
  watchInput,
} from 'modlock'

// The game deletes a solid model that carries physics prop data, so the
// catalog holds only models it keeps.
const catalog: CatalogEntry[] = [
  {
    name: 'Crate',
    category: 'Crates',
    model: 'models/props_industrial/wood_crate_64.vmdl',
  },
  {
    name: 'Big crate',
    category: 'Crates',
    model: 'models/props_industrial/wood_crate_64.vmdl',
    scale: 2,
  },
  {
    name: 'Pallet',
    category: 'Ground',
    model: 'models/props_city/wood_pallet_01a.vmdl',
  },
  {
    name: 'Barrier',
    category: 'Ground',
    model: 'models/props_city/concrete_barrier_01a.vmdl',
  },
  {
    name: 'Bench',
    category: 'Ground',
    model: 'models/props_city/bench01a.vmdl',
  },
  {
    name: 'Ramp',
    category: 'Catwalks',
    model: 'models/props_catwalks/catwalk_ramp_256_128_01.vmdl',
  },
  {
    name: 'Walkway',
    category: 'Catwalks',
    model: 'models/props_catwalks/catwalk_straight_256_01.vmdl',
  },
  {
    name: 'Corner',
    category: 'Catwalks',
    model: 'models/props_catwalks/catwalk_curve_90_01.vmdl',
  },
  {
    name: 'Bounce pad',
    category: 'Fun',
    model: 'models/props_gameplay/cube_100_preview.vmdl',
    scale: 0.5,
    bounce: 900,
  },
]

const dropper = new Dropper(
  catalog,
  loadSpot({ map: 'dl_midtown', objects: [] }),
)
dropper.changed = (spot) =>
  log(`spot: ${spot.objects.length} objects, ${encodeSpot(spot).length} bytes`)

let headless: Headless | undefined

onStart((args) => {
  precache({ resources: [...new Set(catalog.map((entry) => entry.model))] })
  watchInput(dropperButtons)
  if (args.includes('probe')) {
    // An empty server hibernates and runs no frames until a player joins.
    serverCommand('sv_hibernate_when_empty 0')
    headless = new Headless()
  }
})

onFrame((frame) => {
  for (const connection of players()) {
    connection.player.watchMovement(true)
  }
  dropper.frame(frame.movement)
  headless?.tick(frame.timeSeconds)
})

onInput((player, pressed) => dropper.input(player, pressed))

command('build', (player) => dropper.toggle(player))

command('probe', (player, args) => {
  const failures = probe(player, Number(args) || 100)
  log(
    `probe: ${failures.length === 0 ? 'every check held' : failures.join('; ')}`,
  )
})

/** crate is a probe object of model and scale at position. */
function crate(
  position: Vector,
  scale: number,
  model = catalog[0].model,
): SpotObject {
  return {
    model,
    position,
    facing: { pitch: 0, yaw: 0, roll: 0 },
    scale,
    bounce: 0,
    placedBy: 0n,
  }
}

/** lost places each catalog model once at position and returns those the game deleted. */
function lost(position: Vector): string[] {
  return [...new Set(catalog.map((entry) => entry.model))].filter((model) => {
    const spot = loadSpot({
      map: 'dl_midtown',
      objects: [crate(position, 1, model)],
    })
    const kept = spot.objects().length === 1
    spot.clear()
    return !kept
  })
}

/** ahead is the point distance in front of player's hero at its feet, offset to the side. */
function ahead(player: Player, distance: number, side = 0): Vector | undefined {
  const pawn = player.pawn()
  if (!pawn) {
    return undefined
  }
  const yaw = (pawn.eyeAngles.yaw * Math.PI) / 180
  return {
    x: pawn.position.x + Math.cos(yaw) * distance - Math.sin(yaw) * side,
    y: pawn.position.y + Math.sin(yaw) * distance + Math.cos(yaw) * side,
    z: pawn.position.z,
  }
}

/** top traces down onto a placed object and returns the height of its top. */
function top(spot: LoadedSpot, index: number): number | undefined {
  // Find the object placed at this index.
  const placed = spot.objects()[index]
  if (!placed) {
    return undefined
  }

  // Trace down from just above the probe box, which stands 64 tall, so a low
  // ceiling over the spawn does not catch the trace.
  const { x, y, z } = placed.object.position
  const hit = trace({
    start: { x, y, z: z + 96 * placed.object.scale },
    end: { x, y, z },
    exclude: triggers,
  })
  return hit?.entity === placed.entity ? hit.position.z : undefined
}

/** grounded reports whether a trace finds the floor under position. */
function grounded(position: Vector): boolean {
  return (
    trace({
      start: { ...position, z: position.z + 64 },
      end: { ...position, z: position.z - 64 },
    }) !== undefined
  )
}

/**
 * probe places solid crates ahead of player, logs what holds, and returns the
 * checks that failed.
 */
function probe(player: Player, count: number): string[] {
  // Find two crate positions beside each other ahead of the hero.
  const failures: string[] = []
  const small = ahead(player, 200, -80)
  const large = ahead(player, 200, 80)
  if (!small || !large) {
    return ['the player has no hero']
  }

  // Record every catalog model the game deletes when placed.
  for (const model of lost(ahead(player, 600)!)) {
    failures.push(`the game deleted ${model}`)
  }

  // Place a small and a large crate and check that both exist.
  const spot = loadSpot({
    map: 'dl_midtown',
    objects: [crate(small, 1), crate(large, 2)],
  })
  log(`probe: placed ${spot.objects().length} of 2`)
  if (spot.objects().length !== 2) {
    failures.push('a crate was not placed')
  }

  // Check that the large crate's top stands at twice the small crate's height.
  const heights = [top(spot, 0), top(spot, 1)].map((z, i) =>
    z === undefined ? undefined : z - [small, large][i].z,
  )
  log(`probe: crate tops at ${heights.join(' and ')}`)
  if (heights.some((height) => height === undefined)) {
    failures.push('a trace missed a crate')
  } else if (Math.abs(heights[1]! - 2 * heights[0]!) > 2) {
    failures.push('the large crate does not collide at twice the height')
  }

  // Move the small crate up and check that it collides where it moved.
  const first = spot.objects()[0]
  if (first) {
    spot.move(
      first,
      { ...first.object.position, z: first.object.position.z + 64 },
      { pitch: 0, yaw: 45, roll: 0 },
    )
    const moved = top(spot, 0)
    log(`probe: moved crate top at ${moved}`)
    if (
      moved === undefined ||
      Math.abs(moved - small.z - heights[0]! - 64) > 2
    ) {
      failures.push('the moved crate does not collide where it moved')
    }
  }

  // Check that the spot encodes to the same bytes twice, then clear it.
  const encoded = encodeSpot(spot.spot())
  const again = encodeSpot(spot.spot())
  log(`probe: encoding ${encoded.length} bytes`)
  if (
    encoded.length !== again.length ||
    encoded.some((byte, i) => byte !== again[i])
  ) {
    failures.push('the encoding is not deterministic')
  }
  spot.clear()

  // Place count crates in rows of 20 and time the load.
  const origin = ahead(player, 400)!
  const objects = Array.from({ length: count }, (_, i) =>
    crate(
      {
        x: origin.x + (i % 20) * 72 - 720,
        y: origin.y,
        z: origin.z + Math.floor(i / 20) * 64,
      },
      1,
    ),
  )
  const start = Date.now()
  const many = loadSpot({ map: 'dl_midtown', objects })
  log(
    `probe: placed ${many.objects().length} of ${count} in ${Date.now() - start} ms`,
  )
  if (many.objects().length !== count) {
    failures.push(
      `${count - many.objects().length} of ${count} crates were not placed`,
    )
  }

  // Clear the crates and return the failed checks.
  many.clear()
  return failures
}

/** timeout bounds the headless probe in seconds. */
const timeout = 60

/** Step is where the headless probe is. */
type Step = 'seat' | 'spawn' | 'land' | 'done'

/**
 * Headless seats a bot, probes in front of it, then drops it onto a large
 * crate to check that a hero stands on a placed object. It quits the server
 * with PASS or FAIL.
 */
class Headless {
  private step: Step = 'seat'
  private started?: number
  private since = 0
  private tried = 0
  private bot?: Player
  private failures: string[] = []
  private landing?: { spot: LoadedSpot; top: number }

  /** tick advances the probe at frame time seconds. */
  tick(time: number): void {
    this.started ??= time
    if (this.step === 'done') {
      return
    }
    if (time - this.started > timeout) {
      this.finish([...this.failures, `timed out waiting for ${this.step}`])
      return
    }
    switch (this.step) {
      case 'seat':
        if (time - this.since >= 1) {
          this.since = time
          this.bot = addBot({ name: 'Builder', team: 2, hero: 'hero_wraith' })
          if (this.bot) {
            this.step = 'spawn'
          }
        }
        return
      case 'spawn': {
        // The hero needs a moment on the ground before it is measured.
        const pawn = this.bot!.pawn()
        if (!pawn || pawn.health <= 0) {
          this.since = time
          return
        }
        if (time - this.since < 3 || time - this.tried < 1) {
          return
        }
        // Traces work once the game has traced the world itself.
        this.tried = time
        if (!grounded(pawn.position)) {
          return
        }
        this.failures = probe(this.bot!, 100)
        const under = ahead(this.bot!, 300)!
        const spot = loadSpot({ map: 'dl_midtown', objects: [crate(under, 2)] })
        const height = top(spot, 0)
        if (height === undefined) {
          this.finish([...this.failures, 'the landing crate was not hit'])
          return
        }
        this.landing = { spot, top: height }
        this.bot!.teleport(
          { ...under, z: height + 200 },
          { pitch: 0, yaw: 0, roll: 0 },
          { x: 0, y: 0, z: 0 },
        )
        this.since = time
        this.step = 'land'
        return
      }
      case 'land': {
        if (time - this.since < 3) {
          return
        }
        const z = this.bot!.pawn()?.position.z
        log(`probe: hero stands at ${z}, crate top at ${this.landing!.top}`)
        if (z === undefined || Math.abs(z - this.landing!.top) > 16) {
          this.failures.push('the hero did not stand on the crate')
        }
        this.landing!.spot.clear()
        this.finish(this.failures)
      }
    }
  }

  /** finish logs the verdict and quits the server. */
  private finish(failures: string[]): void {
    this.step = 'done'
    for (const failure of failures) {
      log(`probe failed: ${failure}`)
    }
    log(`Dropper probe ${failures.length === 0 ? 'PASS' : 'FAIL'}`)
    serverCommand('quit')
  }
}
