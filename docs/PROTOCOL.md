# LocalCam protocol

The phone and the PC talk over one TLS port (default `47474`), served by `LocalCam.exe`.

## Pairing

The PC generates a random 128-bit token on first run (stored in `HKCU\Software\LocalCam\Token`) and puts it in the
QR code: `https://<pc-ip>:47474/?t=<token>`. The page reads the token from its own URL and presents it when opening
the stream. The page itself is public; the stream needs the token. The tray's "New pairing code" replaces the token:
old links are refused from then on, and the connected phone receives `{"type":"unpaired"}` and is disconnected.

TLS uses a self-signed certificate, created on first run in the user's certificate store (`CN=LocalCam`, RSA 2048,
SHA-256, the PC's IPv4 addresses in the SAN, serverAuth EKU, 825 days). It is recreated when it no longer covers
the PC's current address or has under 30 days left.

## HTTP

| Request | Response |
|---|---|
| `GET /` | The phone page (`web/index.html`, embedded in the exe) |
| `GET /ws?t=<token>` with a WebSocket upgrade | `101`, then the stream below. `403` for a wrong token or an `Origin` other than the page's own |
| a `Host` that isn't an IPv4 address or `localhost` | `421` (DNS rebinding guard; the certificate names only IP addresses) |
| anything else | `404` / `405` |

Every response carries `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`,
`Content-Security-Policy: frame-ancestors 'none'` and `Referrer-Policy: no-referrer`.

A connection has 5 s from connecting to the end of its request head. One address may hold 6 connections at once,
and the server 16 in all; beyond that new connections are closed right away.

## WebSocket stream

One phone streams at a time. A new connection with the right token replaces the current one; the replaced phone
receives `{"type":"replaced"}` and stops instead of reconnecting.

**Phone → PC, binary:** one H.264 access unit per message, Annex B (start codes), keyframes carry SPS and PPS.
The phone drops raw frames instead of queueing when its encoder or socket is backed up, so every sent frame
decodes; there are no timestamps or headers.

**Phone → PC, text (JSON):**

| Message | Meaning |
|---|---|
| `{"type":"pong","t":<t>}` | Echo of a ping, for round-trip time |
| `{"type":"stats","encodeMs":8,"fps":30,"kbps":6100}` | Once a second |
| `{"type":"rotate","rotate":90}` | Set the PC's rotation (degrees clockwise: 0, 90, 180, 270) |
| `{"type":"power","on":false}` | Turn the camera off or on (the PC owns the switch and answers with a `config`) |
| `{"type":"orientation","rotation":0,"flip":false}` | Rotation the encoder reports for its frames (degrees clockwise); the PC applies it |
| `{"type":"paused"}` | The page went to the background, which stops the camera. The PC shows "Camera paused" until frames arrive again |

**PC → phone, text (JSON):**

| Message | Meaning |
|---|---|
| `{"type":"config","width":1920,"height":1080,"fps":30,"rotate":0,"quality":"normal"}` | Sent on connect and when the settings change. The phone asks its camera for the size (always landscape; a portrait output fits the rotated video) and frame rate; `rotate` is the PC's rotation in degrees clockwise; `on`: false means the camera is switched off, so the phone stops its camera but stays connected; `quality` (`low`, `normal`, `high`) sets the bitrate: 0.05, 0.1 or 0.2 bits per pixel per frame, lowered automatically while frames pile up on the phone's socket |
| `{"type":"key"}` | Send a keyframe next (after a decode error) |
| `{"type":"ping","t":<t>}` | Every 2 s |
| `{"type":"replaced"}` | Another phone took over |
| `{"type":"unpaired"}` | The PC has a new pairing token; the phone stops and asks for the new QR code instead of reconnecting |

The PC closes a connection after 10 s without data. The phone reconnects with backoff (1, 2, 4, 5, 5… s) and
starts again with a keyframe.
