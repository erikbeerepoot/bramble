"""Endpoint tests for valve-group CRUD.

Serial-dependent helpers (_node_valve_count, _resolve_node_address) are patched
so the tests don't touch the hub. With the master address unresolved, the mirror
engine stores intent without queueing, which is exactly what we want to assert
the HTTP wiring in isolation.
"""
import pytest

MASTER = "1111111111111111"
ZONE_A = "2222222222222222"
ZONE_B = "3333333333333333"


@pytest.fixture
def groups_client(app_client, monkeypatch):
    import app as flask_app
    monkeypatch.setattr(flask_app, '_node_valve_count', lambda device_id: 4)
    monkeypatch.setattr(flask_app, '_resolve_node_address', lambda device_id: None)
    # The mirror/queue helpers are covered by the engine + DB tests; stub them so
    # these endpoint tests exercise only the HTTP + DB wiring (and don't pull in
    # huey's sqlite store).
    monkeypatch.setattr(flask_app, '_apply_master_diff',
                        lambda db, group, force=False: {'set': 0, 'removed': 0,
                                                        'master_unreachable': True})
    monkeypatch.setattr(flask_app, '_teardown_master_slots', lambda db, group: None)
    monkeypatch.setattr(flask_app, '_queue_master_actuator',
                        lambda db, group, command, duration_seconds=0: None)
    return app_client


def _create(client, name='G', master=MASTER, master_valve=3, members=None):
    if members is None:
        members = [{'zone_device_id': ZONE_A, 'zone_valve': 0}]
    return client.post('/api/valve-groups', json={
        'name': name, 'master_device_id': master,
        'master_valve': master_valve, 'members': members})


class TestValveGroupApi:
    def test_create_and_get(self, groups_client):
        resp = _create(groups_client)
        assert resp.status_code == 201
        group = resp.get_json()
        assert group['name'] == 'G'
        assert group['master_device_id'] == MASTER
        assert group['members'] == [{'zone_device_id': ZONE_A, 'zone_valve': 0}]

        got = groups_client.get(f"/api/valve-groups/{group['id']}")
        assert got.status_code == 200
        assert got.get_json()['id'] == group['id']

    def test_list(self, groups_client):
        _create(groups_client, name='G1', master_valve=3,
                members=[{'zone_device_id': ZONE_A, 'zone_valve': 0}])
        _create(groups_client, name='G2', master=ZONE_B, master_valve=0,
                members=[{'zone_device_id': ZONE_A, 'zone_valve': 1}])
        resp = groups_client.get('/api/valve-groups')
        assert resp.status_code == 200
        assert resp.get_json()['count'] == 2

    def test_master_valve_out_of_range_rejected(self, groups_client):
        resp = _create(groups_client, master_valve=9)  # valve_count mocked to 4
        assert resp.status_code == 400

    def test_master_cannot_be_member(self, groups_client):
        resp = _create(groups_client, master=MASTER, master_valve=0,
                       members=[{'zone_device_id': MASTER, 'zone_valve': 0}])
        assert resp.status_code == 400

    def test_duplicate_zone_membership_rejected(self, groups_client):
        assert _create(groups_client, name='G1').status_code == 201
        # Same zone valve in a second group -> 409.
        resp = _create(groups_client, name='G2', master=ZONE_B, master_valve=0,
                       members=[{'zone_device_id': ZONE_A, 'zone_valve': 0}])
        assert resp.status_code == 409

    def test_master_reuse_rejected(self, groups_client):
        assert _create(groups_client, name='G1', master=MASTER, master_valve=3,
                       members=[{'zone_device_id': ZONE_A, 'zone_valve': 0}]).status_code == 201
        resp = _create(groups_client, name='G2', master=MASTER, master_valve=3,
                       members=[{'zone_device_id': ZONE_B, 'zone_valve': 0}])
        assert resp.status_code == 409

    def test_update(self, groups_client):
        group = _create(groups_client).get_json()
        resp = groups_client.put(f"/api/valve-groups/{group['id']}", json={
            'name': 'Renamed',
            'members': [{'zone_device_id': ZONE_B, 'zone_valve': 2}]})
        assert resp.status_code == 200
        updated = resp.get_json()
        assert updated['name'] == 'Renamed'
        assert updated['members'] == [{'zone_device_id': ZONE_B, 'zone_valve': 2}]

    def test_delete(self, groups_client):
        group = _create(groups_client).get_json()
        assert groups_client.delete(f"/api/valve-groups/{group['id']}").status_code == 200
        assert groups_client.get(f"/api/valve-groups/{group['id']}").status_code == 404

    def test_resync(self, groups_client):
        group = _create(groups_client).get_json()
        resp = groups_client.post(f"/api/valve-groups/{group['id']}/resync")
        assert resp.status_code == 200
        assert resp.get_json()['status'] == 'resynced'
