"""Tests for the Rachio webhook endpoint and its helpers.

The valve helpers (_run_valve_once / _stop_valve_once) are stubbed so these tests
exercise webhook auth, dedup, and zone→action routing in isolation from the hub
and huey queue. The valve helpers themselves are covered elsewhere.
"""
import pytest

RACHIO_DEVICE = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
BRAMBLE_DEVICE = 0x123456789ABCDEF0
SECRET = "top-secret-external-id"
WEBHOOK = "/api/integrations/rachio/webhook"


@pytest.fixture
def rachio_client(app_client, temp_db, monkeypatch):
    import app as flask_app

    monkeypatch.setattr(flask_app.Config, 'RACHIO_WEBHOOK_SECRET', SECRET)
    # Clear the process-global dedup cache so ids don't leak between tests.
    flask_app._RACHIO_SEEN_IDS.clear()

    calls = {'run': [], 'stop': []}

    def fake_run(device_id, valve, duration_seconds, source=None):
        calls['run'].append((device_id, valve, duration_seconds, source))
        return {'task_id': 't', 'command_id': 1, 'address': 2, 'valve': valve,
                'duration_seconds': duration_seconds}

    def fake_stop(device_id, valve, source=None):
        calls['stop'].append((device_id, valve, source))
        return {'task_id': 't', 'command_id': 1, 'address': 2, 'valve': valve}

    monkeypatch.setattr(flask_app, '_run_valve_once', fake_run)
    monkeypatch.setattr(flask_app, '_stop_valve_once', fake_stop)

    # A single enabled mapping: Rachio zone 1 → Bramble valve 0.
    temp_db.upsert_rachio_mapping(RACHIO_DEVICE, 1, BRAMBLE_DEVICE, 0, 900)

    app_client.calls = calls
    return app_client


def _payload(sub_type='ZONE_STARTED', zone=1, event_id='evt-1',
             external_id=SECRET, duration=None):
    body = {
        'type': 'ZONE_STATUS',
        'subType': sub_type,
        'deviceId': RACHIO_DEVICE,
        'zoneNumber': zone,
        'id': event_id,
        'externalId': external_id,
    }
    if duration is not None:
        body['duration'] = duration
    return body


class TestRachioWebhookAuth:
    def test_bad_secret_rejected(self, rachio_client):
        resp = rachio_client.post(WEBHOOK, json=_payload(external_id='wrong'))
        assert resp.status_code == 401
        assert rachio_client.calls['run'] == []

    def test_missing_secret_rejected(self, rachio_client):
        body = _payload()
        del body['externalId']
        resp = rachio_client.post(WEBHOOK, json=body)
        assert resp.status_code == 401


class TestRachioWebhookRouting:
    def test_zone_started_runs_mapped_valve(self, rachio_client):
        resp = rachio_client.post(WEBHOOK, json=_payload())
        assert resp.status_code == 200
        assert resp.get_json()['status'] == 'ran'
        # Falls back to the mapping duration (900) when payload omits duration.
        assert rachio_client.calls['run'] == [(BRAMBLE_DEVICE, 0, 900, 'rachio')]

    def test_zone_started_uses_payload_duration(self, rachio_client):
        rachio_client.post(WEBHOOK, json=_payload(duration=120))
        assert rachio_client.calls['run'] == [(BRAMBLE_DEVICE, 0, 120, 'rachio')]

    def test_zone_stopped_stops_valve(self, rachio_client):
        resp = rachio_client.post(WEBHOOK, json=_payload(sub_type='ZONE_STOPPED'))
        assert resp.status_code == 200
        assert resp.get_json()['status'] == 'stopped'
        assert rachio_client.calls['stop'] == [(BRAMBLE_DEVICE, 0, 'rachio')]

    def test_zone_completed_stops_valve(self, rachio_client):
        rachio_client.post(WEBHOOK, json=_payload(sub_type='ZONE_COMPLETED'))
        assert rachio_client.calls['stop'] == [(BRAMBLE_DEVICE, 0, 'rachio')]

    def test_unmapped_zone_ignored(self, rachio_client):
        resp = rachio_client.post(WEBHOOK, json=_payload(zone=7))
        assert resp.status_code == 200
        assert resp.get_json()['status'] == 'unmapped'
        assert rachio_client.calls['run'] == []

    def test_disabled_mapping_ignored(self, rachio_client, temp_db):
        temp_db.upsert_rachio_mapping(RACHIO_DEVICE, 1, BRAMBLE_DEVICE, 0, 900,
                                      enabled=False)
        resp = rachio_client.post(WEBHOOK, json=_payload())
        assert resp.get_json()['status'] == 'unmapped'
        assert rachio_client.calls['run'] == []

    def test_other_subtype_acknowledged_without_acting(self, rachio_client):
        resp = rachio_client.post(WEBHOOK, json=_payload(sub_type='ZONE_CYCLING'))
        assert resp.status_code == 200
        assert resp.get_json()['status'] == 'ignored'
        assert rachio_client.calls['run'] == []

    def test_duplicate_event_id_acted_once(self, rachio_client):
        first = rachio_client.post(WEBHOOK, json=_payload(event_id='dup'))
        second = rachio_client.post(WEBHOOK, json=_payload(event_id='dup'))
        assert first.get_json()['status'] == 'ran'
        assert second.get_json()['status'] == 'duplicate'
        assert len(rachio_client.calls['run']) == 1


class TestRachioHelpers:
    def test_run_duration_prefers_payload(self):
        import app as flask_app
        assert flask_app._rachio_run_duration({'duration': 120}, 900) == 120

    def test_run_duration_falls_back(self):
        import app as flask_app
        assert flask_app._rachio_run_duration({}, 900) == 900
        assert flask_app._rachio_run_duration({'duration': 0}, 900) == 900

    def test_run_duration_clamped(self):
        import app as flask_app
        assert flask_app._rachio_run_duration({'duration': 99999}, 900) == 7200
