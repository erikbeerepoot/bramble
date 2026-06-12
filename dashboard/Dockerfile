# Multi-stage Dockerfile for Bramble Dashboard
# Produces a lightweight nginx image serving the built SPA

# Stage 1: Build the React application
# Pin the build stage to the runner's NATIVE arch ($BUILDPLATFORM) so the Node/vite
# compile runs natively instead of under QEMU emulation for each target platform. The
# emitted dist/ is static and arch-independent, so only the nginx stage below needs to
# be built per-target-arch. Without this, the emulated arm64 npm build hangs/crawls for
# 30+ minutes on every deploy.
FROM --platform=$BUILDPLATFORM node:20-alpine AS build

WORKDIR /app

# Copy dependency files first for better layer caching
COPY package.json package-lock.json ./
RUN npm ci

# Copy source and build
COPY . .
RUN npm run build

# Stage 2: Serve with nginx
FROM nginx:alpine AS production

# Copy built assets
COPY --from=build /app/dist /usr/share/nginx/html

# Copy custom nginx config
COPY nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 80

CMD ["nginx", "-g", "daemon off;"]
