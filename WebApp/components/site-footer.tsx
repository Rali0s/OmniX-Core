import Link from "next/link"

import { siteConfig } from "@/lib/site-content"

export function SiteFooter() {
  return (
    <footer className="border-t border-white/8">
      <div className="mx-auto flex w-full max-w-6xl flex-col gap-6 px-6 py-8 md:px-8 lg:flex-row lg:items-end lg:justify-between">
        <div className="space-y-2">
          <p className="font-heading text-2xl leading-none tracking-[-0.04em] text-foreground">
            {siteConfig.brandName}
          </p>
          <p className="max-w-xl text-sm leading-7 text-muted-foreground">
            {siteConfig.footerNote}
          </p>
        </div>
        <div className="flex flex-wrap gap-4 text-sm text-muted-foreground">
          {siteConfig.navItems.map((item) => (
            <Link
              key={item.href}
              href={item.href}
              className="hover:text-foreground"
            >
              {item.label}
            </Link>
          ))}
          <Link href="/admin" className="hover:text-foreground">
            Admin
          </Link>
        </div>
      </div>
    </footer>
  )
}
