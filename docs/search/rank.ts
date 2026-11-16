// rank runs each query in queries.json against a built search index, by
// default the site's out/api/search, through the client the search dialog
// uses, and prints where the expected entry first appears among the dialog's
// results. The dialog lists each page's title row and then that page's hits;
// the rank counts the hits only, so rank 1 is the first link under the first
// title, and '-' means not listed. With --write <label> it also stores the ranks in ranks-<label>.json.
// --stock queries with the stock options, as the dialog did before
// searchParams.
//
//   bun search/rank.ts [--index <file>] [--stock] [--write <label>]
import { existsSync, readFileSync, statSync, writeFileSync } from 'node:fs'
import { pathToFileURL } from 'node:url'

import { staticClient } from 'fumadocs-core/search/client/orama-static'

import { searchParams } from '../lib/search'

/** Query is one test query and the link it should find. */
interface Query {
  query: string
  kind: string
  expect: string
}

/** flag returns the value given after a command-line flag. */
function flag(name: string): string | undefined {
  const at = process.argv.indexOf(name)
  return at === -1 ? undefined : process.argv[at + 1]
}

const indexPath = flag('--index') ?? `${import.meta.dirname}/../out/api/search`
const label = flag('--write')
const queries = JSON.parse(
  readFileSync(`${import.meta.dirname}/queries.json`, 'utf8'),
) as Query[]
const client = staticClient({
  from: pathToFileURL(indexPath).href,
  search: process.argv.includes('--stock') ? undefined : searchParams,
})

// Rank each query, 0 when the results lack the link.
const ranks: (Query & { rank: number; first: string })[] = []
for (const q of queries) {
  const hits = (await client.search(q.query)).filter(
    (item) => item.type !== 'page',
  )
  ranks.push({
    ...q,
    rank: hits.findIndex((hit) => hit.url === q.expect) + 1,
    first: hits[0]?.url ?? '',
  })
}

// Print the table beside the stored before ranks, and the counts.
const beforePath = `${import.meta.dirname}/ranks-before.json`
const before = new Map<string, number>(
  existsSync(beforePath)
    ? (JSON.parse(readFileSync(beforePath, 'utf8')) as typeof ranks).map(
        (r) => [r.query, r.rank],
      )
    : [],
)
const show = (rank: number | undefined) =>
  String(rank === undefined ? '' : rank || '-').padStart(6)
console.log(
  `${'before'} ${' now'.padStart(6)}  ${'kind'.padEnd(9)} ${'query'.padEnd(30)} expected link`,
)
for (const r of ranks) {
  console.log(
    `${show(before.get(r.query))} ${show(r.rank)}  ${r.kind.padEnd(9)} ${r.query.padEnd(30)} ${r.expect}`,
  )
}
const count = (rs: typeof ranks) =>
  `top 1 ${rs.filter((r) => r.rank === 1).length}, top 5 ${rs.filter((r) => r.rank >= 1 && r.rank <= 5).length}, not found ${rs.filter((r) => r.rank === 0).length}, of ${rs.length}`
console.log(`identifiers: ${count(ranks.filter((r) => r.kind !== 'prose'))}`)
console.log(`prose:       ${count(ranks.filter((r) => r.kind === 'prose'))}`)
console.log(`index: ${statSync(indexPath).size} bytes`)
if (label)
  writeFileSync(
    `${import.meta.dirname}/ranks-${label}.json`,
    `${JSON.stringify(ranks, null, 2)}\n`,
  )
