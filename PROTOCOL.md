# FreeDisplay wire protocol (v2)

Transport: one TCP connection. The tablet app listens nowhere; it connects to
`127.0.0.1:27184`, which `adb reverse tcp:27184 tcp:27184` tunnels to the
Windows host over USB.

Every message is framed as:

| Field   | Type        | Notes                     |
|---------|-------------|---------------------------|
| type    | u8          | message type              |
| length  | u32 BE      | payload length in bytes   |
| payload | length bytes|                           |

All integers are big-endian. Floats are IEEE-754 32-bit, big-endian.

## Tablet -> host

### 1 HELLO
| Field    | Type     |
|----------|----------|
| magic    | u32 `0x46445350` ("FDSP") |
| version  | u16 (3)  |
| width    | u32 physical screen pixels in the current orientation |
| height   | u32      |
| dpi      | u32      |
| refreshHz| u32 display refresh rate the app selected |
| maxDecodeWidth  | u32 largest picture width the tablet's AVC decoder accepts |
| maxDecodeHeight | u32 largest picture height |
| name     | UTF-8, rest of payload |

The tablet reconnects (sending a new HELLO) when it is rotated.

### 11 ORIENTATION
Host to tablet, sent at the start of each session.

| Field | Type |
|-------|------|
| orientation | u8: 0 follow the tablet (auto-rotate), 1 landscape, 2 portrait |

### 4 INPUT
One Android MotionEvent.

| Field        | Type |
|--------------|------|
| action       | u8: 0 down, 1 move, 2 up, 3 hover-move, 4 hover-exit, 5 cancel |
| actionIndex  | u8 index of the pointer the action applies to |
| count        | u8 number of pointers that follow |

Per pointer:

| Field    | Type |
|----------|------|
| id       | u32 Android pointer id |
| tool     | u8: 0 finger, 1 pen, 2 eraser |
| x, y     | f32 normalised 0..1 over the video image |
| pressure | f32 0..1 |
| tiltX    | f32 degrees -90..90 (positive = right) |
| tiltY    | f32 degrees -90..90 (positive = toward user) |
| rotation | f32 degrees 0..360 |
| buttons  | u8 bit0 = barrel button |

### 6 REQUEST_KEYFRAME
Empty payload. Sent when the decoder (re)starts.

### 7 FRAME_STATS
Sent when a frame comes out of the decoder; the host uses it for latency stats.

| Field    | Type |
|----------|------|
| ptsUs    | u64 pts of the decoded frame |
| decodeUs | u32 time from queueing the frame to decoded output, tablet clock |
| waitUs   | u32 time from receiving the frame to queueing it (waiting for a decoder input buffer) |

### 8 DECODER_INFO
UTF-8 text describing the decoder (name, size, low-latency support).

### 10 PONG
Echoes a PING payload unchanged, as soon as it is read.

## Host -> tablet (continued)

### 9 PING
| Field  | Type |
|--------|------|
| hostUs | u64 host clock when sent; echoed back in PONG to measure the link round trip |

## Host -> tablet

### 2 VIDEO_CONFIG
| Field  | Type |
|--------|------|
| width  | u32 encoded width |
| height | u32 encoded height |
| fps    | u32 stream rate; the tablet matches its screen refresh to it (absent in older hosts) |

### 3 VIDEO_FRAME
| Field | Type |
|-------|------|
| ptsUs | u64 presentation time in microseconds |
| flags | u8 bit0 = keyframe |
| data  | H.264 Annex-B, rest of payload (SPS/PPS in-band on keyframes) |
