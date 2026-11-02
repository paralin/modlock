import { useEffect, useReducer, useRef } from 'react'
import type { Sound } from '../studio.pb.js'
import { route } from './api.js'

// SoundPreview leaves playback, seeking and volume to the browser's media controls.
// A new selection unmounts the player and releases its request and playback.
export function SoundPreview(props: { name: string; sound: Sound }) {
  // Hold the browser player and its failure state for this selection.
  const audio = useRef<HTMLAudioElement>(null)
  const [failed, fail] = useReducer(() => true, false)
  const source = `${route('sound', props.name)}?audio=1`

  // Release playback and pending media requests when the selection leaves.
  useEffect(() => {
    const player = audio.current
    return () => {
      if (!player) return
      player.pause()
      player.removeAttribute('src')
      player.load()
    }
  }, [])

  // Keep unsupported resources readable without playback controls.
  if (props.sound.unavailable)
    return <p className="stage-note">{props.sound.unavailable}</p>

  // The native media controls own playback, seeking and volume.
  return (
    <div className="sound-preview">
      {failed ? (
        <p role="status">
          This sound could not be played.{' '}
          <button className="link" onClick={() => location.reload()}>
            Reload to retry
          </button>
        </p>
      ) : (
        <audio
          ref={audio}
          controls
          preload="metadata"
          src={source}
          aria-label="Sound preview"
          onError={fail}
        />
      )}
      {!failed && (
        <a
          className="link"
          href={source}
          download={`${props.name.slice(props.name.lastIndexOf('/') + 1, -7)}.${props.sound.format === 'MP3' ? 'mp3' : 'wav'}`}
        >
          Download sound
        </a>
      )}
    </div>
  )
}
