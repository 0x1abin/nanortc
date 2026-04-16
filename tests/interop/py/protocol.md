# nanortc_peer_cli wire protocol

Line-oriented text protocol between the pytest harness and the `nanortc_peer_cli`
subprocess. One message per line, terminated by `\n`. All binary or multi-line
payloads are base64-encoded (standard alphabet, no newlines, no padding stripping).

Role: the CLI is always the **answerer**. aiortc (Python) generates offers.

## Commands (pytest → CLI, via stdin)

| Verb            | Args                               | Effect                                                                 |
|-----------------|------------------------------------|------------------------------------------------------------------------|
| `INIT`          | `<port>`                           | Create `nanortc_t`, bind UDP socket on port, register event cb. Emits `READY`. |
| `SET_OFFER`     | `<base64 sdp>`                     | Ingest remote offer, generate local answer. Emits `LOCAL_ANSWER <base64>`. |
| `ADD_CANDIDATE` | `<base64 candidate-line>`          | Add remote ICE candidate (full `candidate:...` SDP line).              |
| `DC_SEND_STRING`| `<id> <base64 utf8>`               | Send UTF-8 string on DataChannel with SCTP stream id.                  |
| `DC_SEND_BINARY`| `<id> <base64>`                    | Send binary on DataChannel with SCTP stream id.                        |
| `SHUTDOWN`      | —                                  | Tear down and exit 0. Emits `SHUTDOWN_ACK` before exiting.             |

## Events (CLI → pytest, via stdout)

| Verb              | Args                                            |
|-------------------|-------------------------------------------------|
| `READY`           | —                                               |
| `LOCAL_ANSWER`    | `<base64 sdp>`                                  |
| `LOCAL_CANDIDATE` | `<base64 candidate-line>`                       |
| `ICE_STATE`       | `new \| checking \| connected \| disconnected \| failed` |
| `CONNECTED`       | —                                               |
| `DC_OPEN`         | `<id> <base64 label>`                           |
| `DC_MESSAGE`      | `<id> STRING\|BINARY <base64>`                  |
| `DC_CLOSE`        | `<id>`                                          |
| `ERROR`           | `<base64 message>`                              |
| `SHUTDOWN_ACK`    | —                                               |

## Notes

- **Base64 alphabet**: RFC 4648 §4 (standard, `+/` + padding `=`). No URL-safe variant.
- **Stream ids**: `DC_SEND_*` commands use the SCTP stream id, which the harness
  learns from the `DC_OPEN` event. For the DataChannel MVP every test has exactly
  one channel, so the harness just remembers the first id.
- **Line length**: commands and events fit in 16 KiB. Offer/answer SDPs on localhost
  are ~1.5 KiB. Payloads larger than 16 KiB will be rejected with an `ERROR` event.
- **Error handling**: if a command fails (parse error, nanortc returns non-OK),
  the CLI emits `ERROR <base64 message>` and keeps running. Fatal conditions
  (bind failed, `INIT` twice) also emit `ERROR` and exit 1.
- **Ordering**: events fire synchronously from the nanortc event callback. The
  harness reads events in a background asyncio task and filters by verb.
- **Logging**: the CLI writes diagnostics to stderr (not stdout) so they don't
  pollute the event stream. pytest captures stderr for failure triage.
