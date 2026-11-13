import type { Root, RootContent } from 'mdast'
import type { MdxJsxFlowElement } from 'mdast-util-mdx-jsx'

import { languageOf } from './language'

/**
 * remarkSamples prepares each <Samples> block for the language switch. It
 * wraps every code fence inside in a <Sample> that names the fence's language,
 * and tells the block which languages it holds, so the page can show the
 * reader's language and say when a block has no sample in it.
 */
export function remarkSamples() {
  return (tree: Root): void => {
    walk(tree.children)
  }
}

/** walk finds the <Samples> elements among nodes and below them. */
function walk(nodes: RootContent[]): void {
  for (const node of nodes) {
    if (node.type === 'mdxJsxFlowElement' && node.name === 'Samples') {
      wrap(node)
      continue
    }
    if ('children' in node) {
      walk(node.children)
    }
  }
}

/** wrap puts each code fence of one <Samples> element in its <Sample>. */
function wrap(samples: MdxJsxFlowElement): void {
  const held: string[] = []
  samples.children = samples.children.map((child) => {
    // Prose between the fences stays where it is.
    if (child.type !== 'code') {
      return child
    }

    // A fence in a language the switch does not know would never show, so
    // it fails the build.
    const language = languageOf(child.lang)
    if (language === undefined) {
      throw new Error(
        `<Samples> holds a ${child.lang ?? 'plain'} code fence; use ts, luau, python or go`,
      )
    }
    held.push(language)
    return {
      type: 'mdxJsxFlowElement',
      name: 'Sample',
      attributes: [{ type: 'mdxJsxAttribute', name: 'lang', value: language }],
      children: [child],
    } satisfies MdxJsxFlowElement
  })
  samples.attributes.push({
    type: 'mdxJsxAttribute',
    name: 'langs',
    value: held.join(' '),
  })
}
