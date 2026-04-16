# browser_interop - Browser WebRTC Interop Test

nanortc DataChannel echo server/client, tested against browser WebRTC.
Validates the full connection lifecycle: **ICE -> DTLS -> SCTP -> DataChannel**.

## Architecture

```
+------------------+         HTTP signaling          +-------------------+
|     Browser      | <--- (signaling_server.py) ---> |   browser_interop |
|  (Chrome/Safari) |         localhost:8765          |   (nanortc + UDP) |
+------------------+                                 +-------------------+
        |                                                     |
        +-------------- UDP (STUN / DTLS / SCTP) -------------+
                             localhost:9999
```

- **signaling_server.py** — Pure-Python HTTP relay, two-peer message queue. Zero dependencies.
- **browser_interop** — C binary using nanortc (mbedtls or openssl), with run_loop for UDP I/O.
- **index.html** — Browser UI served by the signaling server at `http://localhost:8765/`.

## Signaling Protocol

Pure HTTP, no WebSocket. Designed for portability (works on ESP32 `esp_http_client`).

| Endpoint | Method | Description |
|---|---|---|
| `/join` | POST | Register as a peer (max 2), returns `{"id": N}` |
| `/send?id=N` | POST | Relay JSON message to the other peer |
| `/recv?id=N&timeout=T` | GET | Long-poll for messages (204 = timeout) |
| `/leave?id=N` | POST | Unregister peer |

Message format (JSON):
```json
{"type": "offer",     "sdp": "v=0\r\n..."}
{"type": "answer",    "sdp": "v=0\r\n..."}
{"type": "candidate", "candidate": "candidate:..."}
```

## Two Test Modes

### Mode A: nanortc = Answerer (default)

Browser creates the offer and DataChannel; nanortc responds.

```
Browser (offerer)              Signaling Server           nanortc (answerer)
      |                              |                          |
      |--- POST /join -------------->|                          |
      |                              |<------- POST /join ------|
      |                              |                          |
      |--- createOffer() ----------->|                          |
      |--- POST /send {offer} ------>|                          |
      |                              |--- GET /recv ----------->|
      |                              |    {offer}               |
      |                              |                          |-- accept_offer()
      |                              |                          |   DTLS init (server)
      |                              |                          |   generate answer
      |                              |<--- POST /send {answer} -|
      |<-- GET /recv {answer} -------|                          |
      |--- setRemoteDescription() ---|                          |
      |                              |                          |
      | ============ ICE Connectivity Check (STUN) ============ |
      |                              |                          |
      |--- POST /send {candidate} -->|--- GET /recv ----------->|
      |                              |   {candidate}            |-- add_remote_candidate()
      |                              |                          |
      | =================== DTLS Handshake ==================== |
      |   (browser=client)           |       (nanortc=server)   |
      |                              |                          |
      | ==================== SCTP + DCEP ====================== |
      |                              |                          |
      |<================ DataChannel Open =====================>|
      |---- send("hello") ---------> |                          |-- echo back
      |<--- onmessage("hello") ------|                          |
```

**DTLS role**: answerer is `setup:passive` (DTLS server). Role is determined once
in `accept_offer()` — no role switch needed.

### Mode B: nanortc = Offerer (`--offer`)

nanortc creates the offer; browser responds. **This is the critical path for
`dtls_set_role()` testing.**

```
nanortc (offerer)              Signaling Server           Browser (answerer)
      |                              |                          |
      |--- POST /join -------------->|                          |
      |                              |<------- POST /join ------|
      |                              |                          |
      |-- create_offer()             |                          |
      |   DTLS init (tentative)      |                          |
      |   setup:actpass in SDP       |                          |
      |---- POST /send {offer} ----->|                          |
      |                              |--- GET /recv {offer} --->|
      |                              |                          |-- setRemoteDescription()
      |                              |                          |-- createAnswer()
      |                              |                          |   setup:active in answer
      |                              |<--- POST /send {answer} -|
      |<--- GET /recv {answer} ------|                          |
      |                              |                          |
      |-- accept_answer()            |                          |
      |   parse answer SDP           |                          |
      |   remote=active => local=passive?                       |
      |   ** dtls_set_role() **      |                          |
      |   switch DTLS to correct role|                          |
      |                              |                          |
      | ============ ICE Connectivity Check (STUN) ============ |
      |                              |                          |
      | =================== DTLS Handshake ==================== |
      |  (role set by dtls_set_role) |  (browser=client/server) |
      |                              |                          |
      | ==================== SCTP + DCEP ====================== |
      |                              |                          |
      |<================ DataChannel Open =====================>|
      |---- send("hello") ---------> |                          |-- echo back
      |<--- onmessage("hello") ------|                          |
```

**Why this tests `dtls_set_role()`**: `create_offer()` initializes DTLS early
(needs certificate fingerprint for SDP), using a tentative client role.
The actual role is only known after the answer arrives. `accept_answer()` calls
`dtls_set_role()` to switch the crypto provider's internal state machine
(OpenSSL: `SSL_set_accept_state`/`SSL_set_connect_state`; mbedtls:
`mbedtls_ssl_conf_endpoint` + `mbedtls_ssl_session_reset`). If this function
is missing or broken, the DTLS handshake will fail.

## Build & Run

```bash
# Build with mbedtls (default for embedded)
cmake -B build -DNANORTC_CRYPTO=mbedtls -DNANORTC_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)

# Build with openssl (Linux host)
cmake -B build -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)

# Build with audio support
cmake -B build -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON -DNANORTC_FEATURE_AUDIO=ON
cmake --build build -j$(nproc)

# Build with full media (audio + video). Only H.264 is offered by default;
# H.265 is opt-in via -DNANORTC_FEATURE_H265=ON.
cmake -B build -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON \
    -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON
cmake --build build -j$(nproc)

# Build with H.265 enabled (required to use --codec h265)
cmake -B build -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON \
    -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON
cmake --build build -j$(nproc)
```

### Step 1: Start signaling server

```bash
python3 examples/browser_interop/signaling_server.py --port 8765
```

### Step 2: Open browser UI

Navigate to `http://localhost:8765/`

### Step 3a: Test answerer mode

```bash
# Terminal: nanortc as answerer (default)
./build/examples/browser_interop/browser_interop --answer -p 9999

# Browser: select "Offerer", click Connect, send a message
```

### Step 3b: Test offerer mode

```bash
# Terminal: nanortc as offerer
./build/examples/browser_interop/browser_interop --offer -p 9999

# Browser: select "Answerer", click Connect, wait for DataChannel
```

### Step 3c: Test audio sending (answerer mode)

```bash
# Terminal: nanortc as answerer with audio
./build/examples/browser_interop/browser_interop --answer -p 9999 \
    -a examples/sample_data/opusSampleFrames

# Browser: select "Offerer", click Connect → should hear Opus audio
```

### Step 3d: Test audio sending (offerer mode)

```bash
# Terminal: nanortc as offerer with audio
./build/examples/browser_interop/browser_interop --offer -p 9999 \
    -a examples/sample_data/opusSampleFrames

# Browser: select "Answerer", click Connect → should hear Opus audio
```

### Step 3e: Test audio + video sending

```bash
# Terminal: nanortc as answerer with audio + video
./build/examples/browser_interop/browser_interop --answer -p 9999 \
    -a examples/sample_data/opusSampleFrames \
    -v examples/sample_data/h264SampleFrames

# Browser: select "Offerer", click Connect → should see H.264 video and hear Opus audio
```

### Step 3f: Test video only (offerer mode)

```bash
# Terminal: nanortc as offerer with video
./build/examples/browser_interop/browser_interop --offer -p 9999 \
    -v examples/sample_data/h264SampleFrames

# Browser: select "Answerer", click Connect → should see H.264 video
```

### CLI Options

```
  -p PORT        UDP port (default: 9999)
  -b IP          Bind/candidate IP (default: auto-detect)
  -s HOST:PORT   Signaling server (default: localhost:8765)
  -a DIR         Opus frame directory for audio send
  -v DIR         H.264 frame directory for video send
  --offer        Act as offerer (CONTROLLING)
  --answer       Act as answerer (CONTROLLED, default)
```

## Expected Output

### Answerer mode (Test A)

```
[sig] Joined as peer 0 (server localhost:8765)
nanortc browser_interop (mode=answer, udp=192.168.1.100:9999, sig=localhost:8765)
Waiting for SDP offer...
[sig] Got SDP offer (458 bytes)
[sig] Generated SDP answer (467 bytes)
[sig] Sent SDP answer
Entering event loop...
[event] ICE connected
[event] DTLS connected
[event] SCTP connected
[event] DataChannel open (stream=0)
[event] DC string: hello
```

### Offerer mode (Test B)

```
[sig] Joined as peer 1 (server localhost:8765)
nanortc browser_interop (mode=offer, udp=192.168.1.100:9999, sig=localhost:8765)
[sig] Sent SDP offer
Waiting for SDP answer...
[sig] Got SDP answer (435 bytes)
Entering event loop...
[event] ICE connected
[event] DTLS connected            <-- proves dtls_set_role() works
[event] SCTP connected
[event] Created DataChannel 'test' (stream=1)
[event] DataChannel open (stream=1)
[event] DC string: hello
```

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `Join failed (status=409)` | Stale peers in signaling server | `curl -X POST localhost:8765/leave?id=0` and `id=1` |
| `bind: Address already in use` | Previous process still holds UDP port | `lsof -i :9999` then `kill <PID>`, or use `-p <other_port>` |
| Stuck at "ICE connected" | DTLS handshake failure | Check `dtls_set_role` implementation for your crypto backend |
| No ICE candidates | Browser uses mDNS candidates | Ensure STUN server is reachable, or test on same machine |
| `Signaling error waiting for offer` | Browser didn't connect in time | Start nanortc first, then click Connect in browser within ~10s |

## ESP32 Interop

The same signaling server and protocol work with the ESP32 DataChannel example.
The ESP32 acts as answerer while the browser or Linux CLI acts as offerer.

See [`../esp32_datachannel/README.md`](../esp32_datachannel/README.md) for setup instructions.
