// fpv-drone flies a first-person drone on a player's own controllers. A player
// types /drone to launch it above their hero and again to land; a player still
// choosing a hero gets one, and the drone launches once it is alive. Between, their
// view is the drone's camera, steered by throttle, yaw, pitch and roll from a
// gamepad or from a radio transmitter in USB joystick mode, as their "sticks"
// setting assigns. The drone flies in acro mode, so it rolls and loops freely,
// and when the controller stream stops being live it holds its attitude with
// the motors cut.
import {
  command,
  type EntityCamera,
  entityCamera,
  log,
  onFrame,
  type Player,
  type PlayerControllers,
  players,
  toast,
  trace,
} from 'modlock'

import { angles, type Drone, launch, step } from './flight'
import { type Layout, read } from './sticks'

/** Flight is one player's drone and the camera that shows it. */
interface Flight {
  camera: EntityCamera
  drone: Drone
  /** armed turns true once the camera settles and the sticks take over. */
  armed: boolean
  /** logged is the game time of the flight's last log line, in seconds. */
  logged: number
}

/** flights holds each flying player's flight by slot. */
const flights = new Map<number, Flight>()

/** launchHeight is how far above the hero's eyes the drone starts, in inches. */
const launchHeight = 96

/** fov is the camera's horizontal field of view, a wide FPV lens. */
const fov = 110

/** logEvery is how often a flight logs its state, in seconds. */
const logEvery = 2

/** waiting holds the slots of players whose drone launches once their hero is alive. */
const waiting = new Set<number>()

/** pilotHero is the hero a player without one is given to fly from. */
const pilotHero = 'Vindicta'

command('drone', (player) => {
  if (flights.has(player.slot) || waiting.delete(player.slot)) {
    land(player.slot)
    return
  }

  // A player with no hero yet gets one, and flies once it is alive.
  if (!alive(player)) {
    if (player.selectHero(pilotHero, 2) === undefined) {
      toast(player, 'Pick a hero to fly the drone.')
      return
    }
    waiting.add(player.slot)
    return
  }
  takeOff(player)
})

/**
 * alive reports whether player's hero is in play. The game reports a selected
 * hero's spawn before the hero has loaded, so the frame loop watches for it
 * rather than the spawn event.
 */
function alive(player: Player): boolean {
  const pawn = player.pawn()
  return pawn !== undefined && pawn.entity !== 0 && pawn.health > 0
}

/** takeOff launches player's drone above their hero, facing where they look. */
function takeOff(player: Player): void {
  // Start the player's controller stream.
  const pawn = player.pawn()
  if (!pawn) return
  if (!player.watchControllers(true)) {
    toast(player, 'This server cannot read controllers.')
    return
  }

  // Open the camera on a level drone above the hero's eyes.
  const eye = pawn.eyePosition
  const drone = launch(
    { x: eye.x, y: eye.y, z: eye.z + launchHeight },
    pawn.eyeAngles.yaw,
  )
  const camera = entityCamera(player, {
    position: drone.position,
    facing: angles(drone),
    fov,
  })
  if (!camera) {
    player.watchControllers(false)
    toast(player, 'The drone camera could not open.')
    return
  }
  flights.set(player.slot, { camera, drone, armed: false, logged: 0 })
  toast(player, 'Arming…')
}

/** land ends slot's flight: the camera returns to the hero and the watch ends. */
function land(slot: number): void {
  // Return the view to the hero, then end the watch if the player is here.
  const flight = flights.get(slot)
  if (!flight) return
  flights.delete(slot)
  flight.camera.stop()
  const player = players().find((c) => c.player.slot === slot)?.player
  player?.watchControllers(false)
}

/** layout reads player's "sticks" setting, auto when unset or unknown. */
function layout(player: Player): Layout {
  const value = player.setting('sticks')
  return value === 'gamepad' || value === 'aetr' || value === 'taer'
    ? value
    : 'auto'
}

/** lastTime is the previous frame's game time, for the step's length. */
let lastTime: number | undefined

onFrame((frame) => {
  // Step by the frame's length, capped so a hitch does not fling the drone.
  const dt =
    lastTime === undefined
      ? 0
      : Math.min(0.05, Math.max(0, frame.timeSeconds - lastTime))
  lastTime = frame.timeSeconds

  // Land the flights of players who left.
  const present = new Map<number, Player>()
  for (const connection of players())
    present.set(connection.player.slot, connection.player)
  for (const slot of flights.keys()) {
    if (!present.has(slot)) land(slot)
  }

  // Launch the waiting players whose heroes are alive.
  for (const slot of waiting) {
    const player = present.get(slot)
    if (!player) {
      waiting.delete(slot)
    } else if (alive(player)) {
      waiting.delete(slot)
      takeOff(player)
    }
  }

  // Fly each flight on its player's controllers.
  const bySlot = new Map<number, PlayerControllers>()
  for (const entry of frame.controllers) bySlot.set(entry.player.slot, entry)
  for (const [slot, flight] of flights) {
    const player = present.get(slot)
    if (player) fly(player, flight, bySlot.get(slot), frame.timeSeconds, dt)
  }
})

/**
 * fly advances one flight by dt seconds. Until the camera settles it holds
 * the drone level at launch, since the game's opening blend swings a rolled
 * view; from then on the sticks fly it, and the world stops it where it hits.
 */
function fly(
  player: Player,
  flight: Flight,
  entry: PlayerControllers | undefined,
  now: number,
  dt: number,
): void {
  // Hold level until the camera settles, then arm once.
  const { camera, drone } = flight
  if (!camera.settled) {
    camera.move(drone.position, angles(drone))
    return
  }
  if (!flight.armed) {
    flight.armed = true
    toast(player, 'Armed')
  }

  // Move along the step's path, stopping a little off what it hits.
  const sticks = read(entry, layout(player))
  const next = step(drone, sticks, dt)
  const pawn = player.pawn()
  const hit = trace({
    start: drone.position,
    end: next,
    ignore: pawn?.entity ? [pawn.entity] : [],
  })
  if (hit && !hit.startSolid) {
    const n = hit.normal
    drone.position = {
      x: hit.position.x + n.x * 8,
      y: hit.position.y + n.y * 8,
      z: hit.position.z + n.z * 8,
    }
    drone.velocity = { x: 0, y: 0, z: 0 }
  } else {
    drone.position = next
  }
  camera.move(drone.position, angles(drone))

  // Log the flight now and then, for checks that read the server log.
  if (now - flight.logged >= logEvery) {
    flight.logged = now
    const a = angles(drone)
    const p = drone.position
    log(
      `drone: slot ${player.slot} stream ${entry?.stream ?? 'none'} ` +
        `throttle ${sticks.throttle.toFixed(2)} ` +
        `position ${p.x.toFixed(0)} ${p.y.toFixed(0)} ${p.z.toFixed(0)} ` +
        `pitch ${a.pitch.toFixed(0)} yaw ${a.yaw.toFixed(0)} roll ${a.roll.toFixed(0)}`,
    )
  }
}
