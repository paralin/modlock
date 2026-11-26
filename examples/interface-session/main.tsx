// A typed interface backed by disposable text, not a save or addon script.
import { command, log, onPlayerJoin, type Player, show } from 'modlock'

/** sample crosses chunk boundaries with mixed Unicode and line breaks. */
const sample = '雪🙂\n'.repeat(6500)

/** draw shows one player a local settings menu and a document form. */
function draw(
  player: Player,
  mode: 'toggle' | 'hold',
  submitted?: string,
): void {
  show(
    player,
    <menu
      mode={mode}
      openLabel="Text exchange"
      style={{
        horizontalAlign: 'right',
        verticalAlign: 'center',
        margin: [0, 40, 0, 0],
      }}
    >
      <panel
        style={{
          width: 430,
          flow: 'down',
          padding: 20,
          background: '#161b22f5',
          color: '#ffefd7',
        }}
      >
        <label style={{ fontSize: 28, bold: true }}>Text exchange</label>
        <label>
          {submitted === undefined
            ? 'Select and copy the sample, or paste a document to test Submit.'
            : submitted === sample
              ? 'Received the whole sample.'
              : 'Received your document.'}
        </label>
        <documentOutput
          text={submitted ?? sample}
          style={{ width: '100%', margin: [8, 0] }}
        />
        <documentInput
          placeholder="Paste text here"
          submitLabel="Send document"
          style={{ width: '100%' }}
          onSubmit={(sender, text) => {
            log(
              'document received:',
              text === sample ? 'sample' : 'custom',
              text.length,
              'UTF-16 units',
            )
            draw(sender, mode, text)
          }}
        />
        <menuClose
          style={{ padding: [8, 16], margin: [8, 0], background: '#39342d' }}
        >
          Close
        </menuClose>
      </panel>
    </menu>,
  )
}

onPlayerJoin(({ connection }) => draw(connection.player, 'toggle'))
command('holdmenu', (player) => draw(player, 'hold'))
command('togglemenu', (player) => draw(player, 'toggle'))
