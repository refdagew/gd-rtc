# GD-RTC network protocol (draft)

This document defines the initial protocol boundary before transport implementation.

## Transport split

Firebase Realtime Database is used only for room discovery and membership/presence, WebRTC offer/answer exchange, ICE candidate exchange, and small, infrequent level metadata/custom-level transfer coordination.

WebRTC data channels carry gameplay packets directly between peers. Do not write per-frame position updates to RTDB.

## Connectivity policy

GD-RTC intentionally configures no STUN or TURN servers. ICE host candidates are still gathered, but direct connectivity is not guaranteed. UI must report a connection failure and allow retry/leave rather than hang indefinitely.

## Presence lifecycle

Each client creates a random session ID for the current process. A member record contains display name, session ID, last-seen timestamp, current level descriptor, and connection state. Use RTDB onDisconnect cleanup where possible and prune stale records. A room should be removed only after verifying it has no live members; a client must not delete a room merely because its local member list is temporarily empty.

Room codes are identifiers, not authentication. Production RTDB rules must validate writes and membership. Never rely on client-provided user names as proof of account ownership.

## Gameplay packets

Initial packet types: hello (protocol version, session ID, display name and cosmetics); state (sequence number, timestamp, primary position/rotation, game mode, speed, progress, flags, icon IDs and colors); dual_state (second-player state); level (stable level descriptor and joinability); leave (graceful disconnect).

Position state is transient and should use an unreliable, unordered data channel to avoid retransmitting stale frames. Use a separate reliable channel for infrequent control messages and level transfer. Apply input validation, packet-size limits, sequence checks, and interpolation on the receiving side. A 60 Hz send rate is a target, not a guarantee; cap rates and consider bandwidth/CPU cost as room size grows.

## Level descriptors

Use explicit source types such as official, tower, challenge, online, and local-custom. Store enough metadata to identify a level without treating the level name as a unique ID. Custom level sharing needs a transfer ID, size limits, checksum, and explicit handling for missing assets or mod dependencies. Never automatically overwrite an existing local level.

## Compatibility

The packet schema is versioned. Reject incompatible major versions cleanly. Keep Geometry Dash hooks and rendering on the game thread; network callbacks should enqueue state for consumption by the main thread.
