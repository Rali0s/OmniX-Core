import Image from "next/image"
import Link from "next/link"
import { ArrowRight } from "lucide-react"

import { SiteFooter } from "@/components/site-footer"
import { SiteHeader } from "@/components/site-header"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { siteConfig } from "@/lib/site-content"

export default function Page() {
  return (
    <main className="min-h-svh">
      <SiteHeader />
      <div className="mx-auto flex w-full max-w-5xl flex-col gap-14 px-6 pt-10 pb-20 md:px-8 lg:pt-14">
        <section className="space-y-8">
          <div className="mx-auto max-w-3xl space-y-6 text-center">
            <Badge
              variant="outline"
              className="border-primary/25 bg-transparent text-primary"
            >
              {siteConfig.tagline}
            </Badge>
            <div className="space-y-4">
              <h1 className="font-heading text-6xl leading-[0.92] tracking-[-0.05em] text-balance text-foreground sm:text-7xl">
                The Black Lantern Rotation
              </h1>
              <p className="mx-auto max-w-2xl text-base leading-7 text-muted-foreground sm:text-lg">
                You can only be purple team at home for so long. Eventually,
                two people are more than one, and the notes turn into
                dispatches, broadcasts, and books.
              </p>
            </div>
            <div className="flex flex-wrap justify-center gap-3">
              <Button asChild size="lg">
                <Link href="/blog">
                  Read dispatches
                  <ArrowRight />
                </Link>
              </Button>
              <Button asChild size="lg" variant="outline">
                <Link href="/broadcasts">Broadcasts</Link>
              </Button>
            </div>
          </div>

          <div className="mx-auto w-full max-w-3xl">
            <div className="relative aspect-[2/3] overflow-hidden rounded-[1.25rem] border border-white/10 bg-black/50">
              <Image
                src="/black-lantern-cover.png"
                alt="Black Lantern Rotation cover"
                fill
                priority
                className="object-cover object-center"
                sizes="(min-width: 1024px) 48rem, 100vw"
              />
            </div>
          </div>
        </section>
      </div>
      <SiteFooter />
    </main>
  )
}
