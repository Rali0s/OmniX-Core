import Link from "next/link"

import { Button } from "@/components/ui/button"
import { siteConfig } from "@/lib/site-content"

export function SiteHeader() {
  return (
    <header className="sticky top-0 z-40 border-b border-white/8 bg-background/70 backdrop-blur-xl">
      <div className="mx-auto flex w-full max-w-6xl flex-wrap items-center justify-between gap-4 px-6 py-4 md:px-8">
        <Link href="/" className="space-y-1">
          <div className="font-heading text-2xl leading-none tracking-[-0.04em] text-foreground sm:text-3xl">
            {siteConfig.brandMark}
          </div>
          <p className="font-mono text-[10px] tracking-[0.3em] text-primary/80 uppercase">
            {siteConfig.brandSubmark}
          </p>
        </Link>
        <nav className="flex flex-wrap items-center gap-2">
          {siteConfig.navItems.map((item) => (
            <Button key={item.href} asChild variant="ghost" size="sm">
              <Link href={item.href}>{item.label}</Link>
            </Button>
          ))}
          <Button asChild variant="outline" size="sm">
            <Link href="/admin">Admin</Link>
          </Button>
          <Button asChild size="sm" className="ml-1">
            <Link href="/blog">Read the dispatches</Link>
          </Button>
        </nav>
      </div>
    </header>
  )
}
