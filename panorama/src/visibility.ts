// Panorama keeps the visibility a panel last drew when nothing names one.

/** StyledPanel is the part of a game panel clearVisibility writes. */
export interface StyledPanel {
  readonly style: Record<string, string | null>
}

/**
 * clearVisibility hands a panel's visibility back to its style sheets.
 *
 * Writing null to style.visibility clears the inline value, and where the
 * sheets name a visibility for the panel under its classes they decide again
 * in the same frame. Where they name none, as for the top bar's
 * TeamsContainer, Panorama keeps the value the panel last drew, so a panel
 * collapsed inline and then cleared with null alone stays collapsed in every
 * later game. Writing visible first makes that kept value the default, and
 * changes nothing where the sheets decide, so the panel draws as a fresh one
 * would under the same classes. The rules are in ../StockPanel.tla.
 */
export function clearVisibility(panel: StyledPanel): void {
  panel.style.visibility = 'visible'
  panel.style.visibility = null
}
