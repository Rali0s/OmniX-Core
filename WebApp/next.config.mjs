/** @type {import('next').NextConfig} */
const nextConfig = {
  output: "standalone",
  outputFileTracingIncludes: {
    "/*": ["content/blog/**/*"],
  },
}

export default nextConfig
