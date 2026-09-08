# Pocket Soccer

A native C++ Android 10+ local Wi-Fi soccer game: 11v11, 2–22 phones, one
phone per player. The host phone simulates the match; every other phone
renders the host's 20 Hz state snapshots.

## How to play

1. Put every phone on the same Wi-Fi network (a hotspot from one phone also
   works).
2. One phone taps **HOST**, sets the phone count (+/-), and taps
   **START MATCH**.
3. Everyone else taps **JOIN**, waits for the host beacon, and taps **JOIN**
   again. They get a phone number and their characters, then wait for kickoff.
4. Left thumb: virtual stick (touch the left half of the screen) to run. You
   auto-switch to your team's character nearest the ball. Right thumb:
   **KICK** to boot the ball toward the stick direction. 90-second matches,
   then FULL TIME; the host can tap **REMATCH**.

## Match rules

- The match always contains 22 characters: 11 per team.
- Supported phone counts are even values from 2 to 22.
- Phones are split evenly between teams, then each team's 11 characters are distributed across that team's phones.
- Two phones means one phone controls each full team.
- Odd phone counts and values outside 2-22 are rejected.

## Netcode

UDP, all packets start with a 9-byte id:

| id | dir | payload |
|----|-----|---------|
| `PS_LOBBY1` | host → broadcast:41235 | phoneCount, joined (1 Hz) |
| `PS_HELLO1` | client → host:41234 | none (retried 2.5 Hz) |
| `PS_JOINOK` | host → client | phoneCount, phoneId |
| `PS_FULLV1` | host → client | none |
| `PS_STATE1` | host → clients | ball, clock, phase, scores, active chars, 22 x/y (20 Hz) |
| `PS_INPUT1` | client → host | phoneId, dx, dy, kick (30 Hz) |

Clients declare the host lost after 3 s without a snapshot.

## Build

Install Android Studio with SDK 35, NDK, and CMake 3.22.1. Open this folder in Android Studio and run the `app` configuration on an Android 10+ device. Devices must be on the same network for discovery; if broadcast beacons are filtered by a router, use one phone's Wi-Fi hotspot instead.

The native game lives in `app/src/main/cpp`: `main.cpp` (lobby UI, touch,
netcode glue), `game_world.{h,cpp}` (authoritative match sim, host only),
`renderer.{h,cpp}` (EGL + GLES2 batched renderer, 3x5 font in `font.h`),
`game_rules.{h,cpp}` (lobby math), `wifi_session.{h,cpp}` (UDP helper).
