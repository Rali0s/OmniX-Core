import Link from "next/link"
import { ArrowLeft, ArrowUpRight } from "lucide-react"

import { SectionHeading } from "@/components/section-heading"
import { SiteFooter } from "@/components/site-footer"
import { SiteHeader } from "@/components/site-header"
import { Button } from "@/components/ui/button"

export default function RotationPage() {
  return (
    <main className="min-h-svh">
      <SiteHeader />
      <div className="mx-auto flex w-full max-w-3xl flex-col gap-10 px-6 pt-10 pb-20 md:px-8 lg:pt-14">
        <div className="flex flex-wrap items-center justify-between gap-4">
          <SectionHeading
            eyebrow="Rotation"
            title="Black Lantern Rotation"
            description="The first book in the rotation is available now as an ebook on Lulu."
          />
          <Button asChild variant="outline">
            <Link href="/">
              <ArrowLeft />
              Back to lantern
            </Link>
          </Button>
        </div>

        <section className="rounded-[1.25rem] border border-white/10 bg-card/35 px-6 py-10 text-center">
          <div
            aria-hidden="true"
            className="font-heading text-8xl leading-none text-primary sm:text-9xl"
          >
            ♜
          </div>
          <p className="mt-6 text-base leading-7 text-foreground">
            Purchase the ebook edition on Lulu.
          </p>
          <div className="mt-6">
            <Button asChild>
              <a
                href="https://www.lulu.com/shop/michael-anthony/black-lantern-rotation/ebook/product-jemqwn2.html?page=1&pageSize=4"
                target="_blank"
                rel="noreferrer"
              >
                Buy on Lulu
                <ArrowUpRight />
              </a>
            </Button>
          </div>
        </section>
      </div>
      <SiteFooter />
    </main>
  )
}
