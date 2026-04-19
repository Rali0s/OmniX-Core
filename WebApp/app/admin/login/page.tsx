import { redirect } from "next/navigation"

import { LoginForm } from "@/components/admin/login-form"
import { SiteFooter } from "@/components/site-footer"
import { SiteHeader } from "@/components/site-header"
import { Badge } from "@/components/ui/badge"
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card"
import { getAdminDefaults, isAdminAuthenticated } from "@/lib/auth"

type AdminLoginPageProps = {
  searchParams: Promise<{
    next?: string
  }>
}

export default async function AdminLoginPage({
  searchParams,
}: AdminLoginPageProps) {
  if (await isAdminAuthenticated()) {
    redirect("/admin")
  }

  const { next } = await searchParams
  const adminDefaults = getAdminDefaults()

  return (
    <main className="min-h-svh">
      <SiteHeader />
      <div className="mx-auto flex w-full max-w-6xl flex-col gap-8 px-6 pt-10 pb-20 md:px-8 lg:pt-14">
        <div className="grid gap-6 lg:grid-cols-[minmax(0,0.9fr)_minmax(360px,0.8fr)]">
          <div className="space-y-5">
            <Badge variant="outline">Admin Access</Badge>
            <div className="space-y-4">
              <h1 className="font-heading text-5xl leading-none tracking-[-0.04em] text-balance">
                Sign in to manage dispatches and future Black Lantern controls.
              </h1>
              <p className="max-w-xl text-base leading-7 text-muted-foreground">
                This is a small self-managed admin flow. Dispatches are stored
                as markdown files and can be edited directly from the app,
                including on Railway when a persistent volume is attached.
              </p>
            </div>
          </div>
          <Card className="border border-foreground/10 bg-card/85">
            <CardHeader>
              <CardTitle>Admin login</CardTitle>
            </CardHeader>
            <CardContent className="space-y-4">
              {adminDefaults.isProductionLocked ? (
                <div className="rounded-xl border border-red-500/30 bg-red-500/10 p-4 text-sm text-red-900 dark:text-red-100">
                  Production admin login is locked until{" "}
                  <code>SITE_ADMIN_USERNAME</code>,{" "}
                  <code>SITE_ADMIN_PASSWORD</code>, and{" "}
                  <code>SITE_SESSION_SECRET</code> are set.
                </div>
              ) : !adminDefaults.hasCustomCredentials ? (
                <div className="rounded-xl border border-amber-500/30 bg-amber-500/10 p-4 text-sm text-amber-900 dark:text-amber-100">
                  Development defaults are active right now. Before deploying,
                  set <code>SITE_ADMIN_USERNAME</code>,{" "}
                  <code>SITE_ADMIN_PASSWORD</code>, and{" "}
                  <code>SITE_SESSION_SECRET</code>.
                </div>
              ) : null}
              <LoginForm nextPath={next || "/admin"} />
            </CardContent>
          </Card>
        </div>
      </div>
      <SiteFooter />
    </main>
  )
}
