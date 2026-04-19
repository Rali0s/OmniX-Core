import Link from "next/link"
import { ArrowLeft } from "lucide-react"

import { SectionHeading } from "@/components/section-heading"
import { SiteFooter } from "@/components/site-footer"
import { SiteHeader } from "@/components/site-header"
import { Button } from "@/components/ui/button"

export default function BroadcastsPage() {
  return (
    <main className="min-h-svh">
      <SiteHeader />
      <div className="mx-auto flex w-full max-w-3xl flex-col gap-10 px-6 pt-10 pb-20 md:px-8 lg:pt-14">
        <div className="flex flex-wrap items-center justify-between gap-4">
          <SectionHeading
            eyebrow="Broadcasts"
            title="Future podcasts. None right now."
            description="This page is reserved for future audio. When there is something to publish, it will live here."
          />
          <Button asChild variant="outline">
            <Link href="/">
              <ArrowLeft />
              Back to lantern
            </Link>
          </Button>
        </div>

        <section className="rounded-[1.25rem] border border-white/10 bg-card/35 p-6">
          <p className="font-mono text-[11px] tracking-[0.22em] text-primary uppercase">
            Status
          </p>
          <p className="mt-4 text-base leading-7 text-foreground">
            No episodes yet.
          </p>
          <p className="mt-2 text-sm leading-7 text-muted-foreground">
            Check back later for the first broadcast.
          </p>
        </section>
      </div>
      <SiteFooter />
    </main>
  )
}
