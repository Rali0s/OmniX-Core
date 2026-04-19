import { ReactNode } from "react"

import { cn } from "@/lib/utils"

type SectionHeadingProps = {
  eyebrow: string
  title: string
  description: string
  action?: ReactNode
  className?: string
}

export function SectionHeading({
  eyebrow,
  title,
  description,
  action,
  className,
}: SectionHeadingProps) {
  return (
    <div className={cn("max-w-2xl space-y-4", className)}>
      <p className="font-mono text-[11px] tracking-[0.22em] text-muted-foreground uppercase">
        {eyebrow}
      </p>
      <div className="space-y-3">
        <h2 className="font-heading text-3xl leading-none tracking-[-0.04em] text-balance sm:text-4xl">
          {title}
        </h2>
        <p className="text-base leading-7 text-muted-foreground">
          {description}
        </p>
      </div>
      {action ? <div>{action}</div> : null}
    </div>
  )
}
