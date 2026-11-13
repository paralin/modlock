package mod

import uipb "github.com/paralin/modlock/src/wasm/ui"

// ApplyUi sends one change to the interface the player sees. Package ui builds
// the changes; a mod draws with ui.Show rather than calling this.
func ApplyUi(p Player, change *uipb.Change) error {
	return invoke("Ui", &uipb.UiRequest{Player: p.Slot, Change: change}, nil)
}
