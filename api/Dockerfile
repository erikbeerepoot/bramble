# Multi-stage Dockerfile for Bramble REST API
# Supports both development (hot reload) and production modes

FROM python:3.11-slim as base

# Install uv
RUN pip install uv

# Set working directory
WORKDIR /app

# Copy dependency files
COPY pyproject.toml ./

# Install dependencies
RUN uv sync

# Development stage - hot reload enabled
FROM base as development

# Copy source code
COPY . .

# Expose API port
EXPOSE 5000

# Set development environment
ENV DEBUG=True
ENV FLASK_ENV=development

# Run with hot reload (Flask built-in reloader)
CMD ["uv", "run", "python", "app.py"]

# Production stage - optimized
FROM base as production

# Copy source code
COPY . .

# Expose API port
EXPOSE 5000

# Set production environment
ENV DEBUG=False

# Run production server
CMD ["uv", "run", "python", "app.py"]
