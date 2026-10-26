# GD-RTC

Online multiplayer for Geometry Dash using WebRTC for peer-to-peer gameplay data and Firebase Realtime Database for signaling and room presence.

Inspired by the idea of multiplayer Geometry Dash. GD-RTC is intended to use original implementation code.

> **Early development:** the current code is only a first Geode integration/build scaffold. Room creation, Firebase signaling, WebRTC peers, level joining, player rendering, custom-level transfer, and pause-menu integration are not implemented yet.

## Goals

- Geometry Dash 2.2081 and Geode 5.11.2.
- Display Geometry Dash account names when logged in, otherwise the local player name.
- Use RTDB only for room membership, presence, and WebRTC signaling.
- Use WebRTC data channels for gameplay state and player updates.
- Do not configure STUN or TURN servers; direct connections may fail for peers behind restrictive NATs or firewalls.
- Room cleanup when members disconnect or become stale.
- Share player positions, rotations, gameplay state, icon/color data, and dual-player state.
- Join official, Tower, Challenge, online, and shared custom levels where feasible.
- Main-menu and pause-menu entry points.

## Build

GitHub Actions uses the [Geode build action](https://github.com/geode-sdk/build-geode-mod) to build platform artifacts. Open the **Actions** tab and download artifacts after a successful run.

## Firebase

The Firebase configuration identifies the project but is not an authorization mechanism. Configure restrictive Realtime Database rules before enabling room writes. Do not deploy public unauthenticated write access.

## Networking constraint

As requested, GD-RTC will not use STUN or TURN servers. WebRTC can attempt direct host-candidate connectivity, but some peers will be unable to connect depending on NAT/firewall configuration. The mod should report failures clearly.
