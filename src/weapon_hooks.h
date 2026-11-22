#pragma once
#include <cstdint>
#include <string>

struct Features;

class WeaponHooks {
public:
    bool initialize(uintptr_t module);
    void update(const Features& features);
    void requestKillAll();
    void requestAddMoney(int amount);
    void requestAddCoins(int amount);
    void requestAddExperience(int amount);
    void requestFreeHideoutUpgrade();
    void requestSetTimeOfDay(int hour);
    void refreshWorldTime();
    int currentWorldMinute() const;
    int lastKillCount() const;
    std::string status() const;
    void shutdown();
};
