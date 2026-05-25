# Men G2 Counter for M5AtomS3

[日本語版はこちら](README_ja.md)

An Arduino sketch for M5AtomS3 that connects to Even G2 over BLE, authenticates, boots the EvenHub page, and keeps a synchronized counter on both the AtomS3 display and the G2 glasses.

Connects M5AtomS3 to Even G2 directly, sending strings without depending on the Even app, MentraOS app, or smartphone.

The protocol-oriented code derived from the Mentra OS Even G2 implementation is intentionally isolated in G2MentraProtocol.h and G2MentraProtocol.cpp to keep the separation clear.

Almost entirely made by GPT-5.4.

Demo: https://x.com/Seg_Faul/status/2058883443916980324

## Current behavior

- Scans for Even G2 lenses over BLE and connects to the right lens (required)
- Connects to the left lens as well if found
- Performs the startup/authentication sequence based on the Kotlin reference implementation in G2.kt
- Creates an EvenHub text container with event capture enabled
- Displays the current counter value on the AtomS3 screen and inside the G2 glasses
- Increments the counter when M5AtomS3 BtnA is pressed
- Increments the counter when the G2 text container sends a tap event
- Periodically sends EvenHub heartbeat and DevSettings heartbeat packets
- Retries the session after disconnect or startup failure

## Project layout

- men-g2-atoms3-hello.ino: Main sketch for BLE discovery, connection, boot sequence, counter state, local display, and input handling
- G2MentraProtocol.h: Isolated declarations for packet building and parsing derived from the Mentra OS reference
- G2MentraProtocol.cpp: Isolated protocol implementation, including EvenHub text updates and tap-event parsing
- G2_BLE_Investigation_Report.md: Investigation notes for the BLE bring-up and library/runtime mismatches found during debugging
- LICENSE: License for this repository
- MentraOS_LICENSE: Upstream Mentra OS license reference

## Requirements

- M5AtomS3
- Even G2 nearby and disconnected from other apps/devices
- Arduino CLI 1.5.0 or later, or Arduino IDE using the same board package
- M5Stack ESP32 board package with FQBN m5stack:esp32:m5stack_atoms3
- M5Unified library

## Setup

Install the board package and library if needed:

```powershell
arduino-cli core install m5stack:esp32
arduino-cli lib install M5Unified
```

## Build

From the workspace root:

```powershell
arduino-cli compile --fqbn m5stack:esp32:m5stack_atoms3 .
```

## Upload

Find the serial port:

```powershell
arduino-cli board list
```

Then upload:

```powershell
arduino-cli upload -p COM3 --fqbn m5stack:esp32:m5stack_atoms3 .
```

Open a serial monitor at 115200 bps to observe boot, discovery, and input logs.

## Configuration

Adjust the constants near the top of men-g2-atoms3-hello.ino as needed:

- kTargetSerialFragment: Restrict discovery when multiple G2 units are nearby
- kKnownLeftAddress and kKnownRightAddress: Skip scanning and connect to fixed addresses
- kHeartbeatIntervalMs: Heartbeat interval
- kRemoteTapDebounceMs: Debounce window for G2 tap events
- kEnableEspVerboseLogs: Enable verbose ESP and BLE logging for debugging

## Notes

- The current sketch requires the right lens for the EvenHub page and counter updates. The left lens is optional.
- BLE behavior depends on the G2 firmware and the installed M5Stack BLE library.
- The concrete local NimBLE diff used during investigation is stored in [BLEClient_nimble_connect_fix.patch](BLEClient_nimble_connect_fix.patch).
- It targets Arduino15's installed BLEClient.cpp and changes three behaviors: connect() is released on BLE_GAP_EVENT_CONNECT success, m_isConnected remains deferred until connect() itself finishes, and MTU exchange failure no longer aborts the connection immediately.
- Re-check that the patch still applies after board-package updates. See G2_BLE_Investigation_Report.md for the background and debugging history.
- See G2_BLE_Investigation_Report.md for the detailed debugging history and caveats.

## Licensing

This repository is released under the MIT License. See LICENSE.

The Mentra-derived protocol logic is kept in separate files and is based on the Mentra OS Even G2 implementation, which is also MIT-licensed. See MentraOS_LICENSE.
