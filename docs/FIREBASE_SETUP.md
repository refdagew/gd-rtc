# Firebase Realtime Database setup

Deploy the included rules from Firebase Console → Realtime Database → Rules, or use Firebase CLI with `firebase.json` and run `firebase deploy --only database`.

## Security limitation

This prototype has no Firebase Authentication or trusted backend. The rules allow anyone who knows a room code to read and modify that room. They validate expected room/member/state/level fields and deny access to unrelated database paths, but they do not prove that a client owns a peer ID. Do not store private data in rooms, and do not treat room codes as passwords. A production release needs authenticated users and a backend or rules that bind writes to authenticated identities. See https://firebase.google.com/docs/database/security.

## Data layout

- `rooms/{code}`: room metadata and member records
- `rooms/{code}/members/{peerId}`: player display name and presence timestamps
- `rooms/{code}/states/{peerId}`: latest player-state sample
- `rooms/{code}/levels/{levelKey}`: level transfer payloads

The client currently creates/joins rooms and refreshes member names. The state and level paths define a data contract for future gameplay/level-transfer implementation; the paths alone do not mean those features are already wired into the game.
