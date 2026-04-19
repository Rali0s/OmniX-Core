FROM node:22-alpine AS deps
WORKDIR /app/WebApp
COPY WebApp/package.json WebApp/package-lock.json ./
RUN npm ci

FROM node:22-alpine AS builder
WORKDIR /app/WebApp
COPY --from=deps /app/WebApp/node_modules ./node_modules
COPY WebApp ./
RUN npm run build

FROM node:22-alpine AS runner
WORKDIR /app
ENV NODE_ENV=production
ENV HOSTNAME=0.0.0.0

COPY --from=builder /app/WebApp/.next/standalone ./
COPY --from=builder /app/WebApp/.next/static ./.next/static
COPY --from=builder /app/WebApp/public ./public
COPY --from=builder /app/WebApp/content ./content

EXPOSE 3000

CMD ["node", "server.js"]
