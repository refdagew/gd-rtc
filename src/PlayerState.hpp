#pragma once

#include <cstdint>

namespace gdrtc {

// In-memory model only. Never memcpy this type onto the network: padding,
// endianness, and compiler ABI are not stable wire formats.
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 0;

enum class PacketType : std::uint8_t {
    Hello = 1,
    State = 2,
    DualState = 3,
    Level = 4,
    Leave = 5,
};

enum class LevelSource : std::uint8_t {
    None = 0,
    Official = 1,
    Tower = 2,
    Challenge = 3,
    Online = 4,
    LocalCustom = 5,
};

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct PlayerCosmetics {
    std::uint16_t cube = 0;
    std::uint16_t ship = 0;
    std::uint16_t ball = 0;
    std::uint16_t ufo = 0;
    std::uint16_t wave = 0;
    std::uint16_t robot = 0;
    std::uint16_t spider = 0;
    std::uint16_t swing = 0;
    std::uint16_t jetpack = 0;
    std::uint8_t primaryColor = 0;
    std::uint8_t secondaryColor = 0;
    std::uint8_t glowColor = 0;
    bool glow = false;
};

struct PlayerFrame {
    std::uint32_t sequence = 0;
    std::uint64_t timestampMs = 0;
    Vec2 primaryPosition{};
    float primaryRotation = 0.0f;
    Vec2 secondaryPosition{};
    float secondaryRotation = 0.0f;
    bool dual = false;
    bool dead = false;
    bool paused = false;
    bool mini = false;
    bool upsideDown = false;
    bool primaryOnGround = false;
    bool secondaryOnGround = false;
    std::uint8_t gameMode = 0;
    std::uint8_t speed = 0;
    float progress = 0.0f;
    PlayerCosmetics cosmetics{};
};

} // namespace gdrtc
