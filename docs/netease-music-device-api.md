# NetEase Cloud Music device login contract

The manager API base URL is configured by `CONFIG_NETEASE_MUSIC_API_BASE_URL`. When blank, firmware
derives it from the configured OTA URL by removing its trailing `/ota` path. Requests carry the
same short-lived device identity used by the OTA/WebSocket flow: `Device-Id`, `Client-Id`, and the
OTA-issued `Authorization: Bearer ...` token. No NetEase account password, Cookie, refresh token,
access token, or other long-lived credential may be returned to or persisted by the device.

`self.netease_music.login` is also the authoritative login-status query. The conversation must call
it before answering whether the account is logged in, when the user says the account has membership
rights, or when deciding whether an unplayable song is caused by login state. Download failures,
missing audio URLs, copyright restrictions, and membership restrictions must not be used on their
own to claim that the account is logged out; generic device status is not a substitute for this query.
The status endpoint is queried even while a QR session is already waiting for authorization. If the
server reports that authorization has completed before the next polling tick, the device closes the
QR page and returns the logged-in result; a status-query failure keeps the existing QR session alive.

## Required operations

All operations are device-scoped and use the manager-api routes below.

1. `GET /device/netease/status`
   - Request: authenticated device identity only; no user credential fields.
   - Success response: `status`, exactly `logged_in` or `logged_out`.
2. `POST /device/netease/sessions`
   - Request: authenticated device identity only.
   - Success response:
     - `session_id`: non-empty opaque one-time session identifier.
     - `qr_image_url`: HTTPS URL of a PNG/JPEG QR image, at most 256 KiB. It must not embed a
       reusable account credential.
     - `expires_in_ms`: positive session lifetime measured from response receipt. The deployed
       manager currently serializes this integer as a decimal JSON string; firmware accepts both.
     - `poll_interval_ms`: recommended interval, accepted as a JSON integer or decimal string; the
       device clamps it to 500–10000 ms and uses 2000 ms when zero.
3. `GET /device/netease/sessions/{session_id}`
   - Request: authenticated device identity plus the exact opaque `session_id`.
   - Success response: `status`, exactly one of `pending`, `authorized`, `expired`, `cancelled`, or
     `failed`.
   - Optional diagnostic fields may be logged server-side, but must not contain secrets and are
     not required by firmware.
4. `POST /device/netease/logout`
   - Request: authenticated device identity only.
   - Success response: `status`, exactly `logged_out` or `already_logged_out`.

Non-2xx responses, timeouts, malformed JSON, unknown enum values, and transport failures map to a
failed `Result`. A temporary polling failure must not invalidate the server session: firmware keeps
the dedicated QR page visible and continues polling until a terminal server status or the declared
local expiry time.

## Conversation notification

After the original login tool call has returned, terminal states are sent to the conversation
server as an MCP JSON-RPC notification:

```json
{
  "jsonrpc": "2.0",
  "method": "notifications/netease_music/status",
  "params": {"message": "网易云音乐登录成功。", "speak": true}
}
```

The conversation server must render `params.message` through TTS when `speak` is true. Initial login
and logout prompts are the direct MCP tool return values and should be spoken verbatim. This is the
only remaining server behavior needed for asynchronous voice prompts.

## Device retention and UI lifecycle

RAM contains only `session_id`, QR image bytes/URL, expiry time, poll interval, and transient state.
Nothing is written to NVS. The dedicated QR page has no UI timer: it remains visible through pending
and temporary network failures, and closes only on authorization success, cancellation, failure,
server/local expiry, or successful logout.
