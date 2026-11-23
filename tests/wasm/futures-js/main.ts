// futures-js reads two files and writes a log line, first with the calls that
// wait and then with the twins that do not, and logs what each returns. The
// frame makes the calls once; each promise logs when it settles, so the log
// shows which Completed event settled them.
import { log, logAsync, onFrame, readFile, readFileAsync } from 'modlock'

let ran = false

onFrame(() => {
  // Only the first frame makes the calls.
  if (ran) return
  ran = true

  // The calls that wait.
  log(`sync:${readFile('a.txt')}|${readFile('b.txt')}`)

  // The calls that do not wait settle together, and their continuations run
  // as each promise settles.
  const a = readFileAsync('a.txt').then((text) => log(`a:${text}`))
  const b = readFileAsync('b.txt').then((text) => log(`b:${text}`))
  const w = logAsync('written').then((ok) => log(`w:${ok}`))
  void Promise.all([a, b, w]).then(() => log('all'))
})
