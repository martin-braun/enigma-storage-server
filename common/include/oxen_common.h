#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>

#include <oxenmq/hex.h>

namespace oxen {

using namespace std::literals;

using time_point_t = std::chrono::steady_clock::time_point;

inline constexpr size_t MAINNET_USER_PUBKEY_SIZE = 66;
inline constexpr size_t TESTNET_USER_PUBKEY_SIZE = 64;

inline bool is_mainnet = true;

inline size_t get_user_pubkey_size() {
    /// TODO: eliminate the need to check condition every time
    return is_mainnet ? MAINNET_USER_PUBKEY_SIZE : TESTNET_USER_PUBKEY_SIZE;
}

class user_pubkey_t {

    std::string pubkey_;

    user_pubkey_t() {}

    user_pubkey_t(std::string pk) : pubkey_(std::move(pk)) {}

  public:
    static user_pubkey_t create(std::string pk, bool& success) {
        success = true;
        if (pk.size() != get_user_pubkey_size() || !oxenmq::is_hex(pk)) {
            success = false;
            return {};
        }
        return user_pubkey_t(std::move(pk));
    }

    // Returns a reference to the user pubkey hex string, including mainnet prefix if on mainnet
    const std::string& str() const { return pubkey_; }

    // Returns the un-prefixed pubkey hex string
    std::string_view key() const {
        std::string_view r{pubkey_};
        if (is_mainnet)
            r.remove_prefix(2);
        return r;
    }
};

/// message as received from client
struct message_t {
    std::string pub_key;
    std::string data;
    std::string hash;
    std::chrono::milliseconds ttl;
    std::chrono::system_clock::time_point timestamp;
};

using swarm_id_t = uint64_t;

constexpr swarm_id_t INVALID_SWARM_ID = UINT64_MAX;

} // namespace oxen
