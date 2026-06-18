"""Tests for API bearer-token authentication on valve endpoints.

Auth runs in the @require_token decorator before the view body. These tests assert
the decorator's decision only: a rejected request returns 401; an allowed request
reaches the view (which then 404/500s on the unknown test node — anything but 401
proves auth passed).
"""
import pytest

VALVE_RUN = '/api/nodes/123/valve'
VALVE_STOP = '/api/nodes/123/valve/stop'
TOKEN = 'test-secret-token'


@pytest.fixture
def with_token(app_client):
    """Configure API_TOKEN for the duration of a test, then reset enforcement off."""
    app_client.application.config['API_TOKEN'] = TOKEN
    yield TOKEN
    app_client.application.config['API_TOKEN'] = ''


class TestValveAuth:
    """Bearer-token enforcement on the valve run/stop endpoints."""

    def test_no_token_configured_allows_request(self, app_client):
        """Empty API_TOKEN disables enforcement (local/dev behavior)."""
        app_client.application.config['API_TOKEN'] = ''
        response = app_client.post(VALVE_RUN, json={'valve': 0, 'duration_seconds': 60})
        assert response.status_code != 401

    def test_valid_bearer_allowed(self, app_client, with_token):
        """A matching bearer token passes auth."""
        response = app_client.post(
            VALVE_RUN, json={'valve': 0, 'duration_seconds': 60},
            headers={'Authorization': f'Bearer {with_token}'})
        assert response.status_code != 401

    def test_missing_bearer_rejected(self, app_client, with_token):
        """No Authorization header is rejected when a token is configured."""
        response = app_client.post(VALVE_RUN, json={'valve': 0, 'duration_seconds': 60})
        assert response.status_code == 401

    def test_wrong_bearer_rejected(self, app_client, with_token):
        """A non-matching bearer token is rejected."""
        response = app_client.post(
            VALVE_RUN, json={'valve': 0, 'duration_seconds': 60},
            headers={'Authorization': 'Bearer wrong-token'})
        assert response.status_code == 401

    def test_malformed_authorization_rejected(self, app_client, with_token):
        """An Authorization header without the Bearer prefix is rejected."""
        response = app_client.post(
            VALVE_RUN, json={'valve': 0, 'duration_seconds': 60},
            headers={'Authorization': with_token})
        assert response.status_code == 401

    def test_cf_access_assertion_allowed(self, app_client, with_token):
        """A Cloudflare Access-authenticated request (dashboard) passes auth."""
        response = app_client.post(
            VALVE_RUN, json={'valve': 0, 'duration_seconds': 60},
            headers={'Cf-Access-Jwt-Assertion': 'header.payload.signature'})
        assert response.status_code != 401

    def test_stop_endpoint_protected(self, app_client, with_token):
        """The stop endpoint enforces the same token."""
        rejected = app_client.post(VALVE_STOP, json={'valve': 0})
        assert rejected.status_code == 401

        allowed = app_client.post(
            VALVE_STOP, json={'valve': 0},
            headers={'Authorization': f'Bearer {with_token}'})
        assert allowed.status_code != 401
