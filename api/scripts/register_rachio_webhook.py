#!/usr/bin/env python3
"""Register (or list) Rachio webhooks that drive Bramble valves.

Rachio's public API lets you register a webhook URL that it POSTs to on zone
events. This one-shot helper does two things:

  * ``list``     — print your Rachio controllers, their zones (number + name),
                   and the available webhook event-type ids. Use this to find
                   the deviceId and zone numbers for your Bramble mappings.
  * ``register`` — create a ZONE_STATUS webhook pointing at the Bramble API,
                   with the shared ``externalId`` secret Bramble checks on every
                   incoming event.

Auth: pass your Rachio API token via ``--token`` or the ``RACHIO_API_TOKEN``
environment variable (get it from https://app.rach.io → Account Settings → Get
API key).

Examples:
    RACHIO_API_TOKEN=xxxx python register_rachio_webhook.py list
    RACHIO_API_TOKEN=xxxx python register_rachio_webhook.py register \\
        --device-id <controller-uuid> \\
        --url https://api.bramble.ag/api/integrations/rachio/webhook \\
        --secret <RACHIO_WEBHOOK_SECRET>

The ``--secret`` value MUST match ``RACHIO_WEBHOOK_SECRET`` in the Bramble API
container env; it is echoed back to Bramble as the payload ``externalId`` and is
how Bramble authenticates the (otherwise public) webhook.
"""
import argparse
import json
import os
import sys

import requests

RACHIO_BASE = "https://api.rach.io/1/public"


def _headers(token: str) -> dict:
    return {"Authorization": f"Bearer {token}",
            "Content-Type": "application/json"}


def _get(token: str, path: str) -> dict:
    resp = requests.get(f"{RACHIO_BASE}/{path}", headers=_headers(token), timeout=15)
    resp.raise_for_status()
    return resp.json()


def _zone_status_event_type_id(token: str) -> str:
    """Return the numeric id of the ZONE_STATUS webhook event type."""
    types = _get(token, "notification/webhook_event_type")
    for entry in types:
        if entry.get("name") == "ZONE_STATUS":
            return str(entry["id"])
    raise SystemExit("Could not find a ZONE_STATUS webhook event type; "
                     "run `list` to see available types.")


def cmd_list(token: str) -> None:
    """Print controllers, zones, and webhook event types."""
    person_id = _get(token, "person/info")["id"]
    person = _get(token, f"person/{person_id}")

    for device in person.get("devices", []):
        print(f"\nController: {device.get('name')!r}")
        print(f"  deviceId: {device['id']}")
        zones = sorted((z for z in device.get("zones", [])),
                       key=lambda z: z.get("zoneNumber", 0))
        for zone in zones:
            enabled = "" if zone.get("enabled", True) else "  (disabled)"
            print(f"    zone {zone.get('zoneNumber')}: "
                  f"{zone.get('name')!r}{enabled}")

    print("\nWebhook event types:")
    for entry in _get(token, "notification/webhook_event_type"):
        print(f"  {entry['id']}: {entry.get('name')}")


def cmd_register(token: str, device_id: str, url: str, secret: str) -> None:
    """Create a ZONE_STATUS webhook on the given controller."""
    event_type_id = _zone_status_event_type_id(token)
    body = {
        "device": {"id": device_id},
        "externalId": secret,
        "url": url,
        "eventTypes": [{"id": event_type_id}],
    }
    resp = requests.post(f"{RACHIO_BASE}/notification/webhook",
                         headers=_headers(token), data=json.dumps(body), timeout=15)
    if resp.status_code not in (200, 201):
        raise SystemExit(f"Rachio rejected the webhook ({resp.status_code}): "
                         f"{resp.text}")
    print("Webhook registered:")
    print(json.dumps(resp.json() if resp.text else {}, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--token", default=os.environ.get("RACHIO_API_TOKEN", ""),
                        help="Rachio API token (or set RACHIO_API_TOKEN)")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="List controllers, zones, and event types")

    reg = sub.add_parser("register", help="Register a ZONE_STATUS webhook")
    reg.add_argument("--device-id", required=True, help="Rachio controller deviceId")
    reg.add_argument("--url", required=True, help="Bramble webhook URL")
    reg.add_argument("--secret", required=True,
                     help="Shared secret; must equal RACHIO_WEBHOOK_SECRET")

    args = parser.parse_args()
    if not args.token:
        print("Error: provide --token or set RACHIO_API_TOKEN", file=sys.stderr)
        return 2

    if args.command == "list":
        cmd_list(args.token)
    elif args.command == "register":
        cmd_register(args.token, args.device_id, args.url, args.secret)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
