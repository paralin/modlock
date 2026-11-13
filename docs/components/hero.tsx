import { ArrowRight, ArrowUpRight } from 'lucide-react'
import Link from 'next/link'

import { repositoryUrl } from '@/lib/site'

import { LanguageButtons } from './language'

/**
 * Hero opens the home page: what Modlock is, where to start, and the language
 * switch, so the first sample a reader sees is already in their language.
 */
export function Hero() {
  return (
    <section className="hero not-prose">
      <p className="hero-eyebrow">Open source · MIT · Runs on WebAssembly</p>
      <h1 className="hero-title">
        Custom game modes for Deadlock,{' '}
        <span className="hero-accent">in the language you like.</span>
      </h1>
      <p className="hero-lead">
        Write a mod in TypeScript, Luau, Python or Go. Modlock builds it for
        WebAssembly, and the game server runs it in a sandbox, reloading it each
        time you save.
      </p>
      <div className="hero-actions">
        <Link href="/start/install" className="hero-primary">
          Get started
          <ArrowRight className="size-4" aria-hidden="true" />
        </Link>
        <Link href="/start/tutorial" className="hero-secondary">
          Read the tutorial
        </Link>
        <a href={repositoryUrl} className="hero-secondary">
          GitHub
          <ArrowUpRight className="size-4" aria-hidden="true" />
        </a>
      </div>
      <div className="hero-language">
        <span>Show samples in</span>
        <LanguageButtons />
      </div>
    </section>
  )
}
