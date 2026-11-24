#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Features;

enum class HideoutResource { Metal, Oil, Concrete, Food, Count };
enum class Faction { Uics, Vlf, Igc, Count };

class WeaponHooks {
public:
    bool initialize(uintptr_t module);
    void update(const Features& features);
    void requestKillAll();
    void requestAddMoney(int amount);
    void requestAddCoins(int amount);
    void requestAddExperience(int amount);
    void requestAddResource(HideoutResource resource, int amount);
    void requestAddReputation(Faction faction, float points);
    void requestFreeHideoutUpgrade();
    void requestSetTimeOfDay(int hour);
    void requestSetWeather(int index);
    void refreshWorldTime();
    std::vector<std::string> weatherOptions() const;
    std::string weatherStatus() const;
    int currentWorldMinute() const;
    int lastKillCount() const;
    std::string status() const;
    void shutdown();
};
