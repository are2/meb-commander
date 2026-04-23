#!/usr/bin/env python3
"""BLE JSON-RPC client for the MEB preheat ESP32 firmware."""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from typing import Any

try:
    from bleak import BleakClient, BleakScanner
except ImportError as exc:  # pragma: no cover - friendly CLI failure
    raise SystemExit("Missing dependency: install with `python -m pip install bleak`") from exc


DEFAULT_NAME = "MEB-Preheat"
SERVICE_UUID = "7e57c000-f8aa-4a1f-9af3-9c0b7fd90e00"
RX_UUID = "7e57c001-f8aa-4a1f-9af3-9c0b7fd90e00"
TX_UUID = "7e57c002-f8aa-4a1f-9af3-9c0b7fd90e00"


class MebBleClient:
    def __init__(self, client: BleakClient, *, show_events: bool) -> None:
        self.client = client
        self.show_events = show_events
        self._rx_buffer = bytearray()
        self._next_id = 1
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}

    async def start(self) -> None:
        await self.client.start_notify(TX_UUID, self._on_notify)

    async def stop(self) -> None:
        await self.client.stop_notify(TX_UUID)

    async def rpc(self, method: str, params: dict[str, Any] | None = None, timeout: float = 8.0) -> dict[str, Any]:
        request_id = self._next_id
        self._next_id += 1

        request: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            request["params"] = params

        loop = asyncio.get_running_loop()
        future: asyncio.Future[dict[str, Any]] = loop.create_future()
        self._pending[request_id] = future

        payload = (json.dumps(request, separators=(",", ":")) + "\n").encode()
        await self.client.write_gatt_char(RX_UUID, payload, response=True)

        try:
            return await asyncio.wait_for(future, timeout=timeout)
        finally:
            self._pending.pop(request_id, None)

    def _on_notify(self, _sender: int, data: bytearray) -> None:
        self._rx_buffer.extend(data)

        while b"\n" in self._rx_buffer:
            raw, _, rest = self._rx_buffer.partition(b"\n")
            self._rx_buffer = bytearray(rest)

            line = raw.strip()
            if not line:
                continue

            try:
                message = json.loads(line.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                print(line.decode("utf-8", errors="replace"))
                continue

            rpc_id = message.get("id")
            if message.get("jsonrpc") == "2.0" and isinstance(rpc_id, int):
                future = self._pending.get(rpc_id)
                if future and not future.done():
                    future.set_result(message)
                    continue

            if self.show_events:
                print(json.dumps(message, separators=(",", ":")))


async def find_device(name: str, timeout: float) -> Any:
    print(f"Scanning for {name!r}...")

    def matches(device: Any, adv: Any) -> bool:
        if device.name == name:
            return True
        return SERVICE_UUID.lower() in [uuid.lower() for uuid in (adv.service_uuids or [])]

    device = await BleakScanner.find_device_by_filter(matches, timeout=timeout)
    if device is None:
        raise SystemExit(f"Did not find BLE device {name!r}")
    return device


async def connect_target(args: argparse.Namespace) -> BleakClient:
    target = args.address if args.address else await find_device(args.name, args.scan_timeout)
    client = BleakClient(target)
    await client.connect(timeout=args.connect_timeout)
    return client


async def run_once(meb: MebBleClient, args: argparse.Namespace) -> bool:
    if args.enable:
        print(json.dumps(await meb.rpc("heating.set", {"enabled": True}), indent=2))
        return True
    if args.disable:
        print(json.dumps(await meb.rpc("heating.set", {"enabled": False}), indent=2))
        return True
    if args.get:
        print(json.dumps(await meb.rpc("heating.get"), indent=2))
        return True
    if args.info:
        print(json.dumps(await meb.rpc("device.info"), indent=2))
        return True
    if args.uptime:
        print(json.dumps(await meb.rpc("device.uptime"), indent=2))
        return True
    if args.reset:
        print(json.dumps(await meb.rpc("device.reset"), indent=2))
        return True
    if args.diagnostics:
        params = {"limit": args.event_limit} if args.event_limit is not None else None
        print(json.dumps(await meb.rpc("device.diagnostics", params), indent=2))
        return True
    if args.can_diagnostics:
        print(json.dumps(await meb.rpc("device.can_diagnostics"), indent=2))
        return True
    if args.events:
        params = {"limit": args.event_limit} if args.event_limit is not None else None
        print(json.dumps(await meb.rpc("device.events", params), indent=2))
        return True
    if args.interval_ms is not None:
        print(json.dumps(await meb.rpc("telemetry.set_interval", {"ms": args.interval_ms}), indent=2))
        return True
    if args.auto_off_minutes is not None:
        print(json.dumps(await meb.rpc("heating.set_auto_off_timer", {"minutes": args.auto_off_minutes}), indent=2))
        return True
    if args.raw:
        payload = args.raw.strip()
        if not payload.endswith("\n"):
            payload += "\n"
        await meb.client.write_gatt_char(RX_UUID, payload.encode(), response=True)
        await asyncio.sleep(args.raw_wait)
        return True
    return False


async def interactive(meb: MebBleClient) -> None:
    print("Connected. Commands: enable, disable, get, info, uptime, reset, diag, candiag, events [limit], interval <ms>, timer <minutes|off>, raw <json>, quit")

    while True:
        line = await asyncio.to_thread(input, "meb> ")
        line = line.strip()
        if not line:
            continue
        if line in {"quit", "exit"}:
            return

        try:
            if line == "enable":
                response = await meb.rpc("heating.set", {"enabled": True})
            elif line == "disable":
                response = await meb.rpc("heating.set", {"enabled": False})
            elif line == "get":
                response = await meb.rpc("heating.get")
            elif line == "info":
                response = await meb.rpc("device.info")
            elif line == "uptime":
                response = await meb.rpc("device.uptime")
            elif line == "reset":
                response = await meb.rpc("device.reset")
            elif line == "diag":
                response = await meb.rpc("device.diagnostics")
            elif line in {"candiag", "can-diag", "can_diagnostics"}:
                response = await meb.rpc("device.can_diagnostics")
            elif line.startswith("events"):
                parts = line.split(maxsplit=1)
                params = {"limit": int(parts[1])} if len(parts) > 1 else None
                response = await meb.rpc("device.events", params)
            elif line.startswith("interval "):
                response = await meb.rpc("telemetry.set_interval", {"ms": int(line.split(maxsplit=1)[1])})
            elif line.startswith("timer "):
                value = line.split(maxsplit=1)[1].strip().lower()
                minutes = 0 if value in {"off", "disable", "disabled"} else int(value)
                response = await meb.rpc("heating.set_auto_off_timer", {"minutes": minutes})
            elif line.startswith("raw "):
                payload = line[4:].strip()
                if not payload.endswith("\n"):
                    payload += "\n"
                await meb.client.write_gatt_char(RX_UUID, payload.encode(), response=True)
                continue
            else:
                print("Unknown command")
                continue

            print(json.dumps(response, indent=2))
        except Exception as exc:
            print(f"Error: {exc}")


async def main_async(args: argparse.Namespace) -> None:
    client = await connect_target(args)

    try:
        meb = MebBleClient(client, show_events=args.watch or not any(
            [
                args.enable,
                args.disable,
                args.get,
                args.info,
                args.uptime,
                args.reset,
                args.diagnostics,
                args.can_diagnostics,
                args.events,
                args.interval_ms is not None,
                args.auto_off_minutes is not None,
                args.raw,
            ]
        ))
        await meb.start()

        handled = await run_once(meb, args)
        if args.watch and handled:
            print("Watching notifications. Press Ctrl+C to stop.")
            await asyncio.Event().wait()
        if not handled:
            await interactive(meb)
    finally:
        if client.is_connected:
            await client.disconnect()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Talk JSON-RPC to MEB-Preheat over BLE.")
    parser.add_argument("--name", default=DEFAULT_NAME, help=f"BLE device name to scan for, default: {DEFAULT_NAME}")
    parser.add_argument("--address", help="Connect directly to a BLE address instead of scanning by name/service")
    parser.add_argument("--scan-timeout", type=float, default=10.0)
    parser.add_argument("--connect-timeout", type=float, default=10.0)
    parser.add_argument("--watch", action="store_true", help="Print telemetry/events after a one-shot command")

    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--enable", action="store_true", help="Enable heating request")
    actions.add_argument("--disable", action="store_true", help="Disable heating request")
    actions.add_argument("--get", action="store_true", help="Read heating state")
    actions.add_argument("--info", action="store_true", help="Read device information")
    actions.add_argument("--uptime", action="store_true", help="Read uptime and last reset reason")
    actions.add_argument("--reset", action="store_true", help="Reset the device")
    actions.add_argument("--diagnostics", action="store_true", help="Read uptime, heap stats, and recent firmware events")
    actions.add_argument("--can-diagnostics", action="store_true", help="Read live CAN/TWAI controller diagnostics")
    actions.add_argument("--events", action="store_true", help="Read recent firmware diagnostic events")
    actions.add_argument("--interval-ms", type=int, help="Set telemetry interval")
    actions.add_argument(
        "--auto-off-minutes",
        "--timer-minutes",
        dest="auto_off_minutes",
        type=int,
        help="Set heating auto-off timer in minutes; 0 disables the user timer",
    )
    actions.add_argument("--raw", help="Send one raw newline-delimited JSON object")

    parser.add_argument("--event-limit", type=int, help="Number of recent diagnostic events to include")
    parser.add_argument("--raw-wait", type=float, default=1.0, help="Seconds to listen after --raw")
    return parser.parse_args()


def main() -> None:
    try:
        asyncio.run(main_async(parse_args()))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
