import { describe, expect, test } from 'bun:test'

import { parseSheet } from './sheets.js'

describe('parseSheet', () => {
  test('reads a decompiled sheet into ranked stand-in rules', () => {
    const rules = parseSheet(`
      @define Gold: #f2e6c9;
      /* a comment { with braces } */
      @keyframes 'pulse' { 0% { opacity: 0; } 100% { opacity: 1; } }
      Label#Title:hover,.Bag > .slot { color: Gold !important; margin: 0px 4px; }
      Panel:disabled { opacity: 0.5 }
      Button:selected .mark { wash-color: Gold; }
    `)
    expect(rules).toEqual([
      {
        selector: '.p-Label[data-id="Title"]:hover',
        specificity: 10101,
        declarations: [
          ['color', '#f2e6c9'],
          ['margin', '0px 4px'],
        ],
      },
      {
        selector: '.Bag > .slot',
        specificity: 200,
        declarations: [
          ['color', '#f2e6c9'],
          ['margin', '0px 4px'],
        ],
      },
      {
        selector: '.p-Panel.disabled',
        specificity: 101,
        declarations: [['opacity', '0.5']],
      },
      {
        selector: '.p-Button[aria-pressed="true"] .mark',
        specificity: 201,
        declarations: [['wash-color', '#f2e6c9']],
      },
    ])
  })
})
