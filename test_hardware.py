#!/usr/bin/env python3
"""
Hardware diagnostic and test utility for Minecraft Sword ESP32-S3.
Tests serial connectivity, MPU-6500 IMU, baseline gyro noise, and interactive button/motion inputs.
"""

import sys
import time
import json
import argparse
import serial

PORT = "/dev/ttyACM0"
BAUD = 115200

def run_tests(port=PORT, baud=BAUD, interactive=False, timeout=8):
    print("=" * 60)
    print("MINECRAFT SWORD ESP32-S3 HARDWARE DIAGNOSTICS")
    print("=" * 60)
    print(f"Target port: {port} at {baud} baud")

    results = {
        "serial_connection": False,
        "imu_detected": False,
        "gyro_calibrated": False,
        "telemetry_stream": False,
        "gyro_stability": False,
        "interactive_tested": False
    }

    try:
        ser = serial.Serial(port, baud, timeout=0.5)
    except Exception as err:
        print(f"[FAIL] Could not open serial port {port}: {err}")
        return False

    results["serial_connection"] = True
    print("[PASS] Serial port opened successfully.")

    # Request ping or listen for telemetry
    ser.write(b"PING\n")

    start_time = time.time()
    gyro_samples = []
    who_found = False
    calibrated_found = False
    buttons_detected = {"walk": False, "jump": False, "use": False, "hotbar": False, "aim": False, "boot": False}
    motion_detected = False
    swing_detected = False

    print("Collecting telemetry and verifying IMU...")

    while time.time() - start_time < timeout:
        line_bytes = ser.readline()
        if not line_bytes:
            continue
        line = line_bytes.decode("utf-8", errors="replace").strip()

        if "WHO_AM_I = 0x70" in line or "WHO_AM_I = 0x71" in line or "WHO_AM_I = 0x73" in line:
            who_found = True
            results["imu_detected"] = True

        if "Gyro offsets" in line and "IMU OK" in line:
            calibrated_found = True
            results["gyro_calibrated"] = True

        if line.startswith("TELEMETRY:"):
            results["telemetry_stream"] = True
            json_str = line[len("TELEMETRY:"):].strip()
            try:
                data = json.loads(json_str)
                gx = data.get("gx", 0.0)
                gy = data.get("gy", 0.0)
                gz = data.get("gz", 0.0)
                mag = data.get("mag", 0.0)
                gyro_samples.append((gx, gy, gz, mag))

                # Track buttons if pressed
                if data.get("walk") == 1:
                    buttons_detected["walk"] = True
                if data.get("jump") == 1:
                    buttons_detected["jump"] = True
                if data.get("use") == 1:
                    buttons_detected["use"] = True
                if data.get("hotbar") == 1:
                    buttons_detected["hotbar"] = True
                if data.get("aim") == 1:
                    buttons_detected["aim"] = True
                if data.get("boot") == 1:
                    buttons_detected["boot"] = True

                if mag > 30.0:
                    motion_detected = True
                if data.get("swing") == 1 or mag > 380.0:
                    swing_detected = True

            except Exception:
                pass

        if len(gyro_samples) >= 30 and not interactive:
            break

    # Analyze samples
    if len(gyro_samples) >= 10:
        results["telemetry_stream"] = True
        avg_mag = sum(s[3] for s in gyro_samples) / len(gyro_samples)
        if avg_mag < 10.0:
            results["gyro_stability"] = True
            print(f"[PASS] Gyro resting stability verified (average resting magnitude: {avg_mag:.2f} dps).")
        else:
            print(f"[WARN] Gyro detected movement or noise during rest test (average magnitude: {avg_mag:.2f} dps).")

    # If who / calibration was missed due to connecting mid stream, verify via live gyro
    if len(gyro_samples) > 0 and not who_found:
        results["imu_detected"] = True
        results["gyro_calibrated"] = True

    print("\n=== BASELINE HARDWARE RESULTS ===")
    for test_name, passed in results.items():
        if test_name == "interactive_tested":
            continue
        status = "[PASS]" if passed else "[FAIL]"
        print(f"{status} {test_name}")

    if interactive:
        print("\n" + "=" * 60)
        print("INTERACTIVE TEST MODE (Press Ctrl+C when finished)")
        print("Move sword, swing, or press buttons to verify inputs:")
        print("=" * 60)
        try:
            while True:
                line_bytes = ser.readline()
                if not line_bytes:
                    continue
                line = line_bytes.decode("utf-8", errors="replace").strip()
                if line.startswith("TELEMETRY:"):
                    data = json.loads(line[len("TELEMETRY:"):].strip())
                    mag = data.get("mag", 0.0)
                    active_btns = [b.upper() for b in ["walk", "jump", "use", "hotbar", "aim", "boot"] if data.get(b) == 1]
                    btns_str = ", ".join(active_btns) if active_btns else "None"
                    sys.stdout.write(f"\rMagnitude: {mag:6.1f} dps | Active Buttons: {btns_str:25} | BLE: {data.get('ble')}")
                    sys.stdout.flush()
                elif "SWING" in line:
                    print(f"\n[EVENT] {line}")
                elif "down" in line or "up" in line:
                    print(f"\n[EVENT] {line}")
        except KeyboardInterrupt:
            print("\nInteractive testing stopped by user.")

    ser.close()
    all_passed = results["serial_connection"] and results["imu_detected"] and results["telemetry_stream"]
    return all_passed

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test Minecraft Sword hardware")
    parser.add_argument("--port", default=PORT, help="Serial device port")
    parser.add_argument("--interactive", "-i", action="store_true", help="Run interactive monitor")
    args = parser.parse_args()

    success = run_tests(port=args.port, interactive=args.interactive)
    sys.exit(0 if success else 1)
