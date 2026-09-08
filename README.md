# Pocket Soccer

A native C++ Android 10+ local Wi-Fi soccer game foundation.

## Match rules

- The match always contains 22 characters: 11 per team.
- Supported phone counts are even values from 2 to 22.
- Phones are split evenly between teams, then each team's 11 characters are distributed across that team's phones.
- Two phones means one phone controls each full team.
- Odd phone counts and values outside 2-22 are rejected.

## Build

Install Android Studio with SDK 35, NDK, and CMake 3.22.1. Open this folder in Android Studio and run the `app` configuration on an Android 10+ device. Devices must be connected to the same Wi-Fi network. The initial native shell starts a UDP host on port `41234`; the next slice is the lobby UI for choosing host/join and broadcasting the session.

The native rules and Wi-Fi code live in `app/src/main/cpp`. Keep match state authoritative on the host, and send compact input/state packets rather than letting each phone simulate independently.
