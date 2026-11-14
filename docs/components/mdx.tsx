import defaultMdxComponents from 'fumadocs-ui/mdx'
import {
  BookOpen,
  Download,
  GraduationCap,
  Languages,
  Package,
  Rocket,
  ShieldCheck,
  SquareTerminal,
  Swords,
  Zap,
} from 'lucide-react'
import type { MDXComponents } from 'mdx/types'

import { Hero } from './hero'
import { Only, Sample, Samples } from './samples'

/** icons are the icons a content page may put on a card. */
const icons = {
  BookOpen,
  Download,
  GraduationCap,
  Languages,
  Package,
  Rocket,
  ShieldCheck,
  SquareTerminal,
  Swords,
  Zap,
}

/** getMDXComponents returns the components every content page can use. */
export function getMDXComponents(components?: MDXComponents) {
  return {
    ...defaultMdxComponents,
    ...icons,
    Hero,
    Samples,
    Sample,
    Only,
    ...components,
  } satisfies MDXComponents
}

export const useMDXComponents = getMDXComponents

declare global {
  type MDXProvidedComponents = ReturnType<typeof getMDXComponents>
}
