#!/usr/bin/env python3
"""
WebSocket and HTTP bridge for Minecraft Sword HUD.
Reads telemetry from ESP32-S3 over serial and broadcasts it to connected browser clients.
"""

import sys
import os
import json
import asyncio
from aiohttp import web
import serial

PORT = "/dev/ttyACM0"
BAUD = 115200
HTTP_PORT = 8080

clients = set()
ser_instance = None
ser_lock = asyncio.Lock()

async def serial_worker(app):
    global ser_instance
    print(f"Serial worker started for {PORT} at {BAUD} baud.")
    
    while True:
        try:
            if ser_instance is None or not ser_instance.is_open:
                try:
                    ser_instance = serial.Serial(PORT, BAUD, timeout=0.1)
                    print(f"Connected to ESP32-S3 on {PORT}.")
                except Exception as e:
                    await asyncio.sleep(1.0)
                    continue

            # Read available lines
            loop = asyncio.get_event_loop()
            line_bytes = await loop.run_in_executor(None, ser_instance.readline)
            if not line_bytes:
                await asyncio.sleep(0.01)
                continue

            line = line_bytes.decode("utf-8", errors="replace").strip()
            if not line:
                continue

            payload = None
            if line.startswith("TELEMETRY:"):
                json_str = line[len("TELEMETRY:"):].strip()
                try:
                    payload = {"kind": "telemetry", "data": json.loads(json_str)}
                except Exception:
                    pass
            elif "SWING (" in line:
                payload = {"kind": "event", "event": "swing", "raw": line}
            elif " down" in line or " up" in line:
                payload = {"kind": "event", "event": "button", "raw": line}
            elif "Gyro offsets" in line:
                payload = {"kind": "event", "event": "calibrated", "raw": line}
            elif "WHO_AM_I" in line:
                payload = {"kind": "event", "event": "who", "raw": line}
            else:
                payload = {"kind": "log", "text": line}

            if payload and clients:
                msg = json.dumps(payload)
                dead_clients = set()
                for ws in list(clients):
                    try:
                        await ws.send_str(msg)
                    except Exception:
                        dead_clients.add(ws)
                clients.difference_update(dead_clients)

        except Exception as err:
            print(f"Serial loop notice: {err}")
            if ser_instance:
                try:
                    ser_instance.close()
                except Exception:
                    pass
                ser_instance = None
            await asyncio.sleep(1.0)

async def websocket_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    clients.add(ws)
    print("Browser client connected to HUD WebSocket.")

    # Send initial status
    await ws.send_str(json.dumps({
        "kind": "status",
        "serial_connected": ser_instance is not None and ser_instance.is_open,
        "port": PORT
    }))

    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.TEXT:
                try:
                    data = json.loads(msg.data)
                    action = data.get("action")
                    if action == "calibrate" and ser_instance and ser_instance.is_open:
                        ser_instance.write(b"CAL\n")
                    elif action == "ping" and ser_instance and ser_instance.is_open:
                        ser_instance.write(b"PING\n")
                    elif action == "write" and ser_instance and ser_instance.is_open:
                        cmd = data.get("command", "") + "\n"
                        ser_instance.write(cmd.encode("utf-8"))
                except Exception as err:
                    print(f"Error handling message: {err}")
    finally:
        clients.discard(ws)
        print("Browser client disconnected.")
    return ws

def create_app():
    app = web.Application()
    app.router.add_get("/ws", websocket_handler)

    # Static files directory
    static_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hud")
    if not os.path.exists(static_dir):
        static_dir = os.path.dirname(os.path.abspath(__file__))

    async def index(request):
        return web.FileResponse(os.path.join(static_dir, "index.html"))

    app.router.add_get("/", index)
    app.router.add_static("/", static_dir)

    async def on_startup(app):
        app["serial_task"] = asyncio.create_task(serial_worker(app))

    async def on_cleanup(app):
        app["serial_task"].cancel()
        global ser_instance
        if ser_instance and ser_instance.is_open:
            ser_instance.close()

    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    return app

if __name__ == "__main__":
    app = create_app()
    print(f"Starting Minecraft Sword HUD server on http://localhost:{HTTP_PORT}")
    web.run_app(app, host="0.0.0.0", port=HTTP_PORT)
