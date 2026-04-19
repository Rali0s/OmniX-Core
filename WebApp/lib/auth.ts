import crypto from "node:crypto"

import { cookies } from "next/headers"
import { redirect } from "next/navigation"

const SESSION_COOKIE = "black_lantern_admin_session"
const SESSION_TTL_SECONDS = 60 * 60 * 12

function getAdminConfig() {
  const configuredUsername =
    process.env.SITE_ADMIN_USERNAME || process.env.OMNIX_ADMIN_USERNAME
  const configuredPassword =
    process.env.SITE_ADMIN_PASSWORD || process.env.OMNIX_ADMIN_PASSWORD
  const configuredSecret =
    process.env.SITE_SESSION_SECRET || process.env.OMNIX_SESSION_SECRET
  const hasCustomCredentials = Boolean(
    configuredUsername && configuredPassword && configuredSecret
  )
  const isProductionLocked =
    process.env.NODE_ENV === "production" && !hasCustomCredentials

  return {
    username: configuredUsername || "admin",
    password: configuredPassword || "change-me",
    secret:
      configuredSecret ||
      "development-only-secret-change-before-production",
    hasCustomCredentials,
    isProductionLocked,
  }
}

function safeCompare(left: string, right: string) {
  const leftBuffer = Buffer.from(left)
  const rightBuffer = Buffer.from(right)

  if (leftBuffer.length !== rightBuffer.length) {
    return false
  }

  return crypto.timingSafeEqual(leftBuffer, rightBuffer)
}

function signPayload(payload: string, secret: string) {
  return crypto.createHmac("sha256", secret).update(payload).digest("base64url")
}

function encodeSession(username: string, secret: string) {
  const payload = Buffer.from(
    JSON.stringify({
      username,
      exp: Math.floor(Date.now() / 1000) + SESSION_TTL_SECONDS,
    })
  ).toString("base64url")

  const signature = signPayload(payload, secret)
  return `${payload}.${signature}`
}

function decodeSession(token: string, secret: string) {
  const [payload, signature] = token.split(".")

  if (!payload || !signature) {
    return null
  }

  const expectedSignature = signPayload(payload, secret)

  if (!safeCompare(signature, expectedSignature)) {
    return null
  }

  try {
    const parsed = JSON.parse(
      Buffer.from(payload, "base64url").toString("utf8")
    )

    if (
      typeof parsed.username !== "string" ||
      typeof parsed.exp !== "number" ||
      parsed.exp < Math.floor(Date.now() / 1000)
    ) {
      return null
    }

    return parsed as { username: string; exp: number }
  } catch {
    return null
  }
}

export function getAdminDefaults() {
  return getAdminConfig()
}

export async function isAdminAuthenticated() {
  const session = await getAdminSession()
  return Boolean(session)
}

export async function getAdminSession() {
  const admin = getAdminConfig()

  if (admin.isProductionLocked) {
    return null
  }

  const cookieStore = await cookies()
  const raw = cookieStore.get(SESSION_COOKIE)?.value

  if (!raw) {
    return null
  }

  return decodeSession(raw, admin.secret)
}

export async function createAdminSession(username: string) {
  const admin = getAdminConfig()

  if (admin.isProductionLocked) {
    throw new Error(
      "Admin login is locked in production until SITE_ADMIN_USERNAME, SITE_ADMIN_PASSWORD, and SITE_SESSION_SECRET are set."
    )
  }

  const cookieStore = await cookies()
  const token = encodeSession(username, admin.secret)

  cookieStore.set(SESSION_COOKIE, token, {
    httpOnly: true,
    sameSite: "lax",
    secure: process.env.NODE_ENV === "production",
    path: "/",
    maxAge: SESSION_TTL_SECONDS,
  })
}

export async function clearAdminSession() {
  const cookieStore = await cookies()
  cookieStore.delete(SESSION_COOKIE)
}

export async function requireAdmin(nextPath = "/admin") {
  const session = await getAdminSession()

  if (!session) {
    redirect(`/admin/login?next=${encodeURIComponent(nextPath)}`)
  }

  return session
}

export function validateAdminCredentials(username: string, password: string) {
  const admin = getAdminConfig()

  if (admin.isProductionLocked) {
    return false
  }

  return (
    safeCompare(username.trim(), admin.username) &&
    safeCompare(password, admin.password)
  )
}
