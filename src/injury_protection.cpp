#include "injury_protection.h"
#include <windows.h>
#include <cstring>

namespace {
constexpr DWORD verifiedTimestamp = 0x41e01d02;
constexpr DWORD verifiedImageSize = 0x0cf84000;
constexpr size_t gateStride = 64;

struct InjuryEntry {
    uintptr_t slotRva;
    uintptr_t implementationRva;
    std::array<unsigned char, 8> prefix;
};

// Wounded and Fractured, with each class's activation and reapplication slots.
constexpr std::array<InjuryEntry, 4> entries{{
    {0x0a8638e0, 0x0625bb70, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9}},
    {0x0a8638e8, 0x0625be50, {0x40, 0x55, 0x53, 0x48, 0x8b, 0xec, 0x48, 0x83}},
    {0x0a863540, 0x0625b900, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b}},
    {0x0a863548, 0x0625b9a0, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b}},
}};

void writeGate(unsigned char* destination, void* volatile* target, void* original) {
    // Windows x64: RCX is the effect instance. StartGameplayEffect stores the
    // target ability component at effect+0x170 before invoking the virtual.
    // Preserve argument registers and tail-call the game for every other target.
    unsigned char code[] = {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,    // mov rax, &target
        0x48, 0x8b, 0x00,                       // mov rax, [rax]
        0x48, 0x85, 0xc0,                       // test rax, rax
        0x74, 0x0a,                             // je original
        0x48, 0x3b, 0x81, 0x70, 0x01, 0, 0,    // cmp rax, [rcx+0x170]
        0x75, 0x01,                             // jne original
        0xc3,                                   // ret
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,    // original: mov rax, original
        0xff, 0xe0,                             // jmp rax
    };
    static_assert(sizeof(void*) == 8);
    static_assert(sizeof(code) <= gateStride);
    std::memcpy(code + 2, &target, sizeof(target));
    std::memcpy(code + 30, &original, sizeof(original));
    std::memcpy(destination, code, sizeof(code));
}
}

bool InjuryProtection::replace(Slot& slot, bool install) {
    DWORD oldProtection{};
    auto* address = const_cast<void**>(slot.entry);
    if (!VirtualProtect(address, sizeof(void*), PAGE_READWRITE, &oldProtection)) return false;
    void* from = install ? slot.original : slot.replacement;
    void* to = install ? slot.replacement : slot.original;
    const bool changed = InterlockedCompareExchangePointer(slot.entry, to, from) == from;
    DWORD unused{};
    VirtualProtect(address, sizeof(void*), oldProtection, &unused);
    return changed;
}

bool InjuryProtection::initialize() {
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Test_C-Win64-Shipping.exe"));
    if (!image) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew >= 0x1000) return false;
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (pe->Signature != IMAGE_NT_SIGNATURE || pe->FileHeader.TimeDateStamp != verifiedTimestamp ||
        pe->OptionalHeader.SizeOfImage != verifiedImageSize) return false;

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        auto& slot = slots_[i];
        slot.entry = reinterpret_cast<void* volatile*>(image + entry.slotRva);
        slot.original = reinterpret_cast<void*>(image + entry.implementationRva);
        if (*slot.entry != slot.original ||
            std::memcmp(slot.original, entry.prefix.data(), entry.prefix.size()) != 0) return false;
    }

    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto pageSize = system.dwPageSize;
    auto* pages = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 2 * pageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!pages) return false;
    targets_ = reinterpret_cast<void* volatile*>(pages + pageSize);
    for (size_t i = 0; i < slots_.size(); ++i) {
        auto* gate = pages + i * gateStride;
        writeGate(gate, targets_ + i / 2, slots_[i].original);
        slots_[i].replacement = gate;
    }
    DWORD oldProtection{};
    if (!VirtualProtect(pages, pageSize, PAGE_EXECUTE_READ, &oldProtection) ||
        !FlushInstructionCache(GetCurrentProcess(), pages, pageSize)) {
        targets_ = nullptr;
        VirtualFree(pages, 0, MEM_RELEASE);
        return false;
    }

    // Retain the two pages after publishing any of the entries. A game thread
    // can have loaded an entry immediately before it is restored on unload.
    for (auto& slot : slots_) {
        if (!replace(slot, true)) {
            const bool published = slots_.front().installed;
            shutdown();
            if (!published) VirtualFree(pages, 0, MEM_RELEASE);
            return false;
        }
        slot.installed = true;
    }
    return true;
}

void InjuryProtection::update(uintptr_t localComponent, bool preventBleeding, bool preventFracture) {
    const bool enabled = preventBleeding || preventFracture;
    status_ = nullptr;
    if (enabled && localComponent && !attempted_) {
        attempted_ = true;
        initialize();
    }
    if (!targets_) {
        if (enabled) status_ = attempted_ ? "无法启用伤害状态保护" : "等待主角进入关卡";
        return;
    }
    InterlockedExchangePointer(targets_, preventBleeding ? reinterpret_cast<void*>(localComponent) : nullptr);
    InterlockedExchangePointer(targets_ + 1, preventFracture ? reinterpret_cast<void*>(localComponent) : nullptr);
    if (enabled && !localComponent) status_ = "等待主角进入关卡";
}

void InjuryProtection::shutdown() {
    if (!targets_) return;
    InterlockedExchangePointer(targets_, nullptr);
    InterlockedExchangePointer(targets_ + 1, nullptr);
    for (auto& slot : slots_) {
        if (slot.installed) replace(slot, false);
        slot.installed = false;
    }
    targets_ = nullptr;
}
