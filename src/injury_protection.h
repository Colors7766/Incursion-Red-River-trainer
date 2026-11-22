#pragma once
#include <array>
#include <cstdint>

class InjuryProtection {
public:
    void update(uintptr_t localComponent, bool preventBleeding, bool preventFracture);
    void shutdown();
    const char* status() const { return status_; }

private:
    struct Slot {
        void* volatile* entry{};
        void* original{};
        void* replacement{};
        bool installed{};
    };

    static bool replace(Slot& slot, bool install);
    bool initialize();
    std::array<Slot, 4> slots_{};
    void* volatile* targets_{};
    const char* status_{};
    bool attempted_{};
};
