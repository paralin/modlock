import { Cog } from 'lucide-react'

import { siteName } from '@/lib/site'

/** Logo is the gear mark and name shown at the top of the navigation. */
export function Logo() {
  return (
    <span className="inline-flex items-center gap-2 font-semibold tracking-tight">
      <span className="bg-fd-primary text-fd-primary-foreground inline-flex size-6 items-center justify-center rounded-md">
        <Cog className="size-4" aria-hidden="true" />
      </span>
      {siteName}
    </span>
  )
}
