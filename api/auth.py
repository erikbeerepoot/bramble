"""Authentication helpers for the Bramble API."""
import hmac
from functools import wraps

from flask import current_app, request, jsonify


def _token_matches(provided: str, expected: str) -> bool:
    """Constant-time comparison of bearer tokens."""
    return hmac.compare_digest(provided, expected)


def require_token(view):
    """Require a valid bearer token on a mutating endpoint.

    Enforcement is active only when ``API_TOKEN`` is configured (non-empty). When
    it is unset the endpoint stays open — preserving the local/dev "no auth,
    local network use" behavior.

    When ``API_TOKEN`` is set, a request is authorized if either:

    1. it carries ``Authorization: Bearer <API_TOKEN>`` (the iOS widget / any
       non-browser client), or
    2. it carries Cloudflare Access's ``Cf-Access-Jwt-Assertion`` header — i.e. a
       dashboard user already authenticated by Cloudflare Access at the edge.
       Flask is only reachable through the Cloudflare tunnel, so the presence of
       this header is trusted in v1; a hardening follow-up validates its
       signature against Cloudflare's JWKS.
    """
    @wraps(view)
    def wrapper(*args, **kwargs):
        expected = current_app.config.get('API_TOKEN', '') or ''
        if not expected:
            return view(*args, **kwargs)

        # Already-authenticated Cloudflare Access browser request (dashboard).
        if request.headers.get('Cf-Access-Jwt-Assertion'):
            return view(*args, **kwargs)

        header = request.headers.get('Authorization', '')
        prefix = 'Bearer '
        if header.startswith(prefix) and _token_matches(header[len(prefix):], expected):
            return view(*args, **kwargs)

        return jsonify({'error': 'Unauthorized'}), 401

    return wrapper
