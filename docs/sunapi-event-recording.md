# SUNAPI Event Recording Design

## Source

Reference document:

```text
SUNAPI_Complete_Guide_2.6.8.pdf
```

Relevant sections found locally:

```text
158.20 Samples / MotionDetection
161.1 Event Status
256.4 Event Session
```

Important PDF pages from local extraction:

```text
PDF page 1635: eventsources.cgi samples / MotionDetection
PDF page 1934: eventstatus description and actions
PDF page 1939: supported Channel EventType list
PDF page 1948: example status response
PDF page 1953: monitor/monitordiff response body example
PDF page 1962: SchemaBased event response
PDF page 2463-2464: VMS event session note
```

## Correct Product Model

`probe` is not a product feature name. The product concept is `ChannelIngest`.

```text
ChannelIngest
  camera RTSP connection stays open
  RTP depay and codec parse stay active
  recording branch is controlled by policy/event state
  live branch is controlled by client sessions
  playback branch serves recorded media independently
```

Do not connect to the camera only after motion starts. That loses pre-event frames
and may wait for the next keyframe.

## SUNAPI Event Source

The SUNAPI guide describes:

```text
/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=check
/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=monitor
/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=monitordiff
```

The guide says:

```text
check       current status once
monitor     status periodically and when state changes
monitordiff initial status, then updates when state changes
```

For our VMS event gate, `monitordiff` is the preferred first target.

SUNAPI channel numbers belong to the camera-facing namespace. For the first
channel, `Channel.0` maps to `camera_channel = 0`, while the VMS, DB, storage,
and client-facing identifier is `channel_id = 1`. Do not derive either value
from the other; store the mapping explicitly in channel configuration.

Motion-only filter:

```text
/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=monitordiff&Channel.0.EventType=MotionDetection
```

Schema based response is recommended by the guide for newer SUNAPI versions:

```text
&SchemaBased=True
```

First implementation can support text response parsing because examples show
simple key-value lines:

```text
Channel.0.MotionDetection=False
Channel.0.MotionDetection.RegionID.1=False
Channel.0.MotionDetection.RegionID.1.Level=0
```

Later implementation should add `SchemaBased=True` parsing.

## Motion Samples

Motion level samples are available through:

```text
/stw-cgi/eventsources.cgi?msubmenu=samples&action=check&EventSourceType=MotionDetection&MaxSamples=5&Channel=0&Index=1
```

This is useful for diagnostics or UI tuning, not the primary event trigger.

## Recording Policy

Recommended policy fields:

```text
mode = continuous | event | disabled
pre_event_seconds = 5
post_event_seconds = 10
record_profile = record
live_profile = default | mobile
```

Modes:

```text
continuous
  ChannelIngest records all parsed video.

event
  ChannelIngest stays connected, but permanent recording is gated by SUNAPI
  motion_start/motion_stop state.

disabled
  ChannelIngest may still be active for live sessions, but recording is off.
```

## Pre/Post Event Recording

Event-only recording needs pre and post windows.

Option A:

```text
memory pre-buffer
  lower disk writes
  harder to implement
  good long-term target
```

Option B:

```text
rolling temporary segments
  easier to implement with splitmuxsink
  writes continuously
  acceptable on USB SSD/HDD
  not acceptable on SD card
```

Initial event recording should use rolling temporary segments only when real
external storage is mounted.

## Timeline Semantics

Store event state transitions separately from video segments.

```text
vms_events
  motion start
  motion stop
  event source
  source event name
  optional metadata JSON
```

UI behavior:

```text
recorded segment exists:
  play actual media

no segment and no event:
  show "no motion / no recording"

event exists but segment missing:
  show event marker with missing media warning
```

Avoid making a still frame look like actual video. If the UI shows the last frame,
label the region as not recorded.

## GStreamer Shape

Long-term shape:

```text
rtspsrc
  -> depay
  -> parse
  -> tee
       -> recording branch
       -> live branch
       -> optional app branch for metadata/timing
```

Branch policies:

```text
recording queue:
  no dropping
  preserve media integrity

live queue:
  leaky allowed
  prefer low latency and latest frames
```

Queue does not solve VPN bandwidth or CPU limits. It only isolates branch
backpressure.

## Next Implementation Steps

1. Add `SunapiEventClient` skeleton.
2. Implement Digest-capable HTTP GET helper.
3. Implement `eventstatus.cgi?action=check` first.
4. Implement long-lived `action=monitordiff` parser.
5. Convert `MotionDetection False -> True` into `motion_start`.
6. Convert `MotionDetection True -> False` into `motion_stop`.
7. Insert rows into `vms_events`.
8. Add `RecordingPolicy` state machine.
9. Then connect event state to GStreamer recording branch.
