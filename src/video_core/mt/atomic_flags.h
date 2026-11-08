// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <type_traits>
#include <utility>

namespace VideoCommon::MT {

/// Drop-in for an enum-flag field (DECLARE_ENUM_FLAG_OPERATORS) whose bits other threads read:
/// `flags |= X`, `flags &= ~X` are atomic read-modify-writes and `True(flags & X)` reads with
/// acquire. Copying (moving an object that holds it) copies the current value.
template <class E>
    requires std::is_enum_v<E>
class AtomicEnumFlags {
    using U = std::underlying_type_t<E>;

public:
    constexpr AtomicEnumFlags() noexcept = default;
    constexpr AtomicEnumFlags(E value) noexcept : bits{static_cast<U>(value)} {}
    AtomicEnumFlags(const AtomicEnumFlags& other) noexcept : bits{other.bits.load(std::memory_order_acquire)} {}
    AtomicEnumFlags& operator=(const AtomicEnumFlags& other) noexcept {
        bits.store(other.bits.load(std::memory_order_acquire), std::memory_order_release);
        return *this;
    }
    AtomicEnumFlags& operator=(E value) noexcept {
        bits.store(static_cast<U>(value), std::memory_order_release);
        return *this;
    }

    [[nodiscard]] E Load(std::memory_order order = std::memory_order_acquire) const noexcept {
        return static_cast<E>(bits.load(order));
    }

    operator E() const noexcept {
        return Load();
    }
    explicit operator U() const noexcept {
        return bits.load(std::memory_order_acquire);
    }

    E operator|=(E value) noexcept {
        return static_cast<E>(bits.fetch_or(static_cast<U>(value), std::memory_order_acq_rel) |
                              static_cast<U>(value));
    }
    E operator&=(E value) noexcept {
        return static_cast<E>(bits.fetch_and(static_cast<U>(value), std::memory_order_acq_rel) &
                              static_cast<U>(value));
    }
    E operator^=(E value) noexcept {
        return static_cast<E>(bits.fetch_xor(static_cast<U>(value), std::memory_order_acq_rel) ^
                              static_cast<U>(value));
    }

    // Explicit binary operators so `flags & X` works without relying on the implicit conversion
    // finding the enum's own operators.
    [[nodiscard]] friend E operator&(const AtomicEnumFlags& lhs, E rhs) noexcept {
        return static_cast<E>(static_cast<U>(lhs.Load()) & static_cast<U>(rhs));
    }
    [[nodiscard]] friend E operator&(E lhs, const AtomicEnumFlags& rhs) noexcept {
        return static_cast<E>(static_cast<U>(lhs) & static_cast<U>(rhs.Load()));
    }
    [[nodiscard]] friend E operator|(const AtomicEnumFlags& lhs, E rhs) noexcept {
        return static_cast<E>(static_cast<U>(lhs.Load()) | static_cast<U>(rhs));
    }
    [[nodiscard]] friend E operator|(E lhs, const AtomicEnumFlags& rhs) noexcept {
        return static_cast<E>(static_cast<U>(lhs) | static_cast<U>(rhs.Load()));
    }
    [[nodiscard]] friend E operator~(const AtomicEnumFlags& value) noexcept {
        return static_cast<E>(~static_cast<U>(value.Load()));
    }
    [[nodiscard]] friend bool operator==(const AtomicEnumFlags& lhs, E rhs) noexcept {
        return lhs.Load() == rhs;
    }

private:
    std::atomic<U> bits{};
};

/// Relaxed atomic counter that stays copyable (for structs moved between containers).
template <class T>
class CopyableAtomic {
public:
    constexpr CopyableAtomic() noexcept = default;
    constexpr CopyableAtomic(T value) noexcept : atom{value} {}
    CopyableAtomic(const CopyableAtomic& other) noexcept : atom{other.atom.load(std::memory_order_acquire)} {}
    CopyableAtomic& operator=(const CopyableAtomic& other) noexcept {
        atom.store(other.atom.load(std::memory_order_acquire), std::memory_order_release);
        return *this;
    }
    CopyableAtomic& operator=(T value) noexcept {
        atom.store(value, std::memory_order_release);
        return *this;
    }

    [[nodiscard]] T load(std::memory_order order = std::memory_order_acquire) const noexcept {
        return atom.load(order);
    }
    void store(T value, std::memory_order order = std::memory_order_release) noexcept {
        atom.store(value, order);
    }
    T fetch_add(T value, std::memory_order order = std::memory_order_acq_rel) noexcept {
        return atom.fetch_add(value, order);
    }
    operator T() const noexcept {
        return load();
    }

private:
    std::atomic<T> atom{};
};

} // namespace VideoCommon::MT
