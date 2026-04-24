#!/usr/bin/env python3
"""Chunked JSON-RPC OTA updater for MEB-Preheat over USB-UART or BLE."""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import json
import sys
import time
import traceback
from pathlib import Path
from typing import Any, Protocol


DEFAULT_NAME = "MEB-Preheat"
SERVICE_UUID = "7e57c000-f8aa-4a1f-9af3-9c0b7fd90e00"
RX_UUID = "7e57c001-f8aa-4a1f-9af3-9c0b7fd90e00"
TX_UUID = "7e57c002-f8aa-4a1f-9af3-9c0b7fd90e00"
MAX_RAW_CHUNK = 576
DEFAULT_RAW_CHUNK = 384


class OtaRpcTimeoutError(TimeoutError):
    pass


def format_duration(seconds: float) -> str:
    total_seconds = max(0, int(round(seconds)))
    minutes, secs = divmod(total_seconds, 60)
    hours, minutes = divmod(minutes, 60)
    if hours > 0:
        return f"{hours:d}h{minutes:02d}m{secs:02d}s"
    if minutes > 0:
        return f"{minutes:d}m{secs:02d}s"
    return f"{secs:d}s"


class RpcTransport(Protocol):
    async def rpc(self, method: str, params: dict[str, Any] | None = None, timeout: float = 8.0) -> dict[str, Any]:
        ...

    async def close(self) -> None:
        ...


def check_rpc_response(response: dict[str, Any]) -> dict[str, Any]:
    if "error" in response:
        raise RuntimeError(json.dumps(response["error"], separators=(",", ":")))
    result = response.get("result")
    if not isinstance(result, dict):
        raise RuntimeError(f"Unexpected JSON-RPC response: {response!r}")
    return result


class SerialRpcTransport:
    def __init__(self, port: str, baud: int, timeout: float) -> None:
        try:
            import serial
        except ImportError as exc:  # pragma: no cover - friendly CLI failure
            raise SystemExit("Missing dependency: install with `python -m pip install pyserial`") from exc

        self.serial = serial.Serial(port, baudrate=baud, timeout=timeout)
        self._next_id = 1

    async def rpc(self, method: str, params: dict[str, Any] | None = None, timeout: float = 8.0) -> dict[str, Any]:
        request_id = self._next_id
        self._next_id += 1
        request: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            request["params"] = params

        payload = (json.dumps(request, separators=(",", ":")) + "\n").encode("utf-8")
        self.serial.write(payload)
        self.serial.flush()

        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout

        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise OtaRpcTimeoutError(f"Timed out waiting for {method} response")

            line = await asyncio.to_thread(self.serial.readline)
            if not line:
                await asyncio.sleep(0)
                continue

            try:
                message = json.loads(line.decode("utf-8").strip())
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue

            if message.get("jsonrpc") == "2.0" and message.get("id") == request_id:
                return message

    async def close(self) -> None:
        self.serial.close()


class BleRpcTransport:
    def __init__(self, client: Any, write_chunk_size: int, write_with_response: bool) -> None:
        self.client = client
        self.write_chunk_size = write_chunk_size
        self.write_with_response = write_with_response
        self._rx_buffer = bytearray()
        self._next_id = 1
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}

    async def start(self) -> None:
        await self.client.start_notify(TX_UUID, self._on_notify)

    async def rpc(self, method: str, params: dict[str, Any] | None = None, timeout: float = 8.0) -> dict[str, Any]:
        request_id = self._next_id
        self._next_id += 1
        request: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            request["params"] = params

        loop = asyncio.get_running_loop()
        future: asyncio.Future[dict[str, Any]] = loop.create_future()
        self._pending[request_id] = future

        payload = (json.dumps(request, separators=(",", ":")) + "\n").encode("utf-8")
        for offset in range(0, len(payload), self.write_chunk_size):
            await self.client.write_gatt_char(
                RX_UUID,
                payload[offset:offset + self.write_chunk_size],
                response=self.write_with_response,
            )

        try:
            return await asyncio.wait_for(future, timeout=timeout)
        except asyncio.TimeoutError as exc:
            raise OtaRpcTimeoutError(f"Timed out waiting for {method} response") from exc
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
                continue

            rpc_id = message.get("id")
            if message.get("jsonrpc") == "2.0" and isinstance(rpc_id, int):
                future = self._pending.get(rpc_id)
                if future and not future.done():
                    future.set_result(message)

    async def close(self) -> None:
        if self.client.is_connected:
            await self.client.disconnect()


async def find_ble_device(name: str, timeout: float) -> Any:
    try:
        from bleak import BleakScanner
    except ImportError as exc:  # pragma: no cover - friendly CLI failure
        raise SystemExit("Missing dependency: install with `python -m pip install bleak`") from exc

    print(f"Scanning for {name!r}...")

    def matches(device: Any, adv: Any) -> bool:
        if device.name == name:
            return True
        return SERVICE_UUID.lower() in [uuid.lower() for uuid in (adv.service_uuids or [])]

    device = await BleakScanner.find_device_by_filter(matches, timeout=timeout)
    if device is None:
        raise SystemExit(f"Did not find BLE device {name!r}")
    return device


async def connect_transport(args: argparse.Namespace) -> RpcTransport:
    if args.serial:
        return SerialRpcTransport(args.serial, args.baud, args.serial_timeout)

    try:
        from bleak import BleakClient
    except ImportError as exc:  # pragma: no cover - friendly CLI failure
        raise SystemExit("Missing dependency: install with `python -m pip install bleak`") from exc

    target = args.address if args.address else await find_ble_device(args.name, args.scan_timeout)
    client = BleakClient(target)
    await client.connect(timeout=args.connect_timeout)
    transport = BleRpcTransport(client, args.ble_write_chunk, args.ble_write_with_response)
    await transport.start()
    return transport


async def update_firmware(transport: RpcTransport, firmware: Path, args: argparse.Namespace) -> None:
    image = firmware.read_bytes()
    image_size = len(image)
    image_sha256 = hashlib.sha256(image).hexdigest()

    if image_size == 0:
        raise SystemExit("Firmware image is empty")

    print(f"Image: {firmware}")
    print(f"Size: {image_size} bytes")
    print(f"SHA-256: {image_sha256}")

    print("Starting OTA session...")
    begin = check_rpc_response(await transport.rpc(
        "firmware.begin",
        {"size": image_size, "sha256": image_sha256},
        timeout=args.rpc_timeout,
    ))
    max_base64_chars = int(begin.get("max_base64_chars", 768))
    max_raw = min(args.chunk_size, (max_base64_chars // 4) * 3)
    if max_raw <= 0:
        raise RuntimeError(f"Device reported invalid chunk limit: {begin!r}")

    start_time = time.perf_counter()
    chunk_count = 0
    written = int(begin.get("written", 0))
    while written < image_size:
        chunk = image[written:written + max_raw]
        data = base64.b64encode(chunk).decode("ascii")
        try:
            result = check_rpc_response(await transport.rpc(
                "firmware.write",
                {"offset": written, "data": data},
                timeout=args.rpc_timeout,
            ))
        except OtaRpcTimeoutError as exc:
            raise OtaRpcTimeoutError(f"{exc} at offset {written}") from exc
        written = int(result["written"])
        chunk_count += 1
        elapsed = time.perf_counter() - start_time
        rate_bps = written / elapsed if elapsed > 0 else 0.0
        remaining_seconds = ((image_size - written) / rate_bps) if rate_bps > 0 else 0.0
        print(
            f"\rWritten {written}/{image_size} bytes ({written * 100 // image_size}%) "
            f"at {rate_bps / 1024:.1f} KiB/s, ETA {format_duration(remaining_seconds)}",
            end="",
            flush=True,
        )

    print()
    print("Finalizing OTA image...")
    end = check_rpc_response(await transport.rpc("firmware.end", timeout=args.rpc_timeout))
    elapsed = time.perf_counter() - start_time
    rate_bps = image_size / elapsed if elapsed > 0 else 0.0
    print("Update finalized:")
    print(json.dumps(end, indent=2))
    print(
        "Transfer stats: "
        f"{chunk_count} chunks in {elapsed:.2f}s, "
        f"{rate_bps / 1024:.1f} KiB/s average"
    )

    if args.reboot:
        print("Rebooting device...")
        response = check_rpc_response(await transport.rpc("firmware.reboot", timeout=args.rpc_timeout))
        print(json.dumps(response, indent=2))
    else:
        print("Reboot not requested. Use firmware.reboot or reset the device to boot the new image.")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Update MEB-Preheat firmware over JSON-RPC OTA.")
    parser.add_argument("firmware", type=Path, help="Path to the ESP-IDF app .bin file")

    transport = parser.add_mutually_exclusive_group(required=True)
    transport.add_argument("--serial", metavar="PORT", help="USB-to-UART serial port, for example COM5")
    transport.add_argument("--ble", action="store_true", help="Use BLE transport")

    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--serial-timeout", type=float, default=0.2, help="Serial read timeout in seconds")
    parser.add_argument("--name", default=DEFAULT_NAME, help=f"BLE device name, default: {DEFAULT_NAME}")
    parser.add_argument("--address", help="BLE address instead of scanning by name/service")
    parser.add_argument("--scan-timeout", type=float, default=10.0)
    parser.add_argument("--connect-timeout", type=float, default=10.0)
    parser.add_argument("--ble-write-chunk", type=int, default=180, help="Maximum bytes per BLE GATT write")
    parser.add_argument(
        "--ble-write-with-response",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Use acknowledged BLE writes for RX fragments; disable with --no-ble-write-with-response",
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=DEFAULT_RAW_CHUNK,
        help=f"Raw firmware bytes per RPC chunk, default {DEFAULT_RAW_CHUNK}, max {MAX_RAW_CHUNK}",
    )
    parser.add_argument("--rpc-timeout", type=float, default=20.0)
    parser.add_argument("--reboot", action="store_true", help="Reboot into the new image after finalizing")
    parser.add_argument("--debug", action="store_true", help="Print Python traceback on failure")

    args = parser.parse_args()
    if args.chunk_size < 1 or args.chunk_size > MAX_RAW_CHUNK:
        parser.error(f"--chunk-size must be 1..{MAX_RAW_CHUNK}")
    if args.ble_write_chunk < 20:
        parser.error("--ble-write-chunk must be at least 20")
    if not args.firmware.is_file():
        parser.error(f"Firmware image not found: {args.firmware}")
    return args


async def main_async() -> None:
    args = parse_args()
    transport = await connect_transport(args)
    try:
        await update_firmware(transport, args.firmware, args)
    finally:
        await transport.close()


def main() -> None:
    try:
        asyncio.run(main_async())
    except KeyboardInterrupt:
        sys.exit(130)
    except Exception as exc:
        if "--debug" in sys.argv:
            traceback.print_exc()
        message = str(exc) or exc.__class__.__name__
        if "GetOverlappedResult failed" in message or "Access is denied" in message:
            message += (
                "\nSerial port access failed. Close ESP-IDF Monitor, VS Code serial monitor, "
                "or any other program using the COM port, then retry."
            )
        raise SystemExit(f"OTA update failed: {message}") from exc


if __name__ == "__main__":
    main()
