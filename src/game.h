#pragma once
#include <windows.h>
#include <array>
#include <utility>
#include <vector>
#include <string>
#include <unordered_map>

struct Vec3 { double x{}, y{}, z{}; };
struct Rotator { double pitch{}, yaw{}, roll{}; };
struct BoneTransform {
    double rotation[4]{};
    Vec3 translation{};
    double translationPadding{};
    Vec3 scale{};
    double scalePadding{};
};
static_assert(sizeof(BoneTransform) == 0x60);
struct CameraState { Vec3 location{}; Rotator rotation{}; float fov{90.f}; bool valid{}; };
struct Target {
    uintptr_t actor{};
    Vec3 root{};
    Vec3 meshOrigin{};
    Vec3 head{};
    float distance{};
    float health{}, maxHealth{};
    bool healthValid{};
    bool dead{};
    POINT rootScreen{};
    POINT headScreen{};
    std::array<POINT, 10> aimScreen{};
    std::array<bool, 10> aimValid{};
    std::array<Vec3, 10> aimWorld{};
    POINT boxMin{}, boxMax{};
    bool boxValid{};
    int boneCount{};
    std::vector<std::pair<POINT, POINT>> skeletonLines;
    bool visible{};
};

struct Features {
    bool espBox{true};
    bool espSkeleton{true};
    float skeletonColor[4]{1.f, 230.f / 255.f, 80.f / 255.f, 240.f / 255.f};
    bool aimbot{false};
    bool showAimFov{true};
    float espDistance{200.f};
    int aimBone{9};
    int aimKey{VK_RBUTTON};
    bool captureAimKey{false};
    float aimDistance{100.f};
    float aimFov{8.f};
    float boxColor[4]{1.f, 70.f / 255.f, 70.f / 255.f, 230.f / 255.f};
    bool espDistanceLabel{true};
    bool espHealthBar{true};
    bool espHealthNumber{true};
    bool espCorpse{true};
    float corpseDistance{200.f};
    bool infiniteHealth{};
    bool infiniteBodyStamina{};
    bool infiniteArmStamina{};
    bool preventBleeding{};
    bool preventFracture{};
    bool keepInventoryOnDeath{};
    float playerSpeedMultiplier{1.f};
    float playerJumpHeightMultiplier{1.f};
    float gameSpeedMultiplier{1.f};
    int teleportKey{VK_F6};
    bool captureTeleportKey{false};
    bool weaponInfiniteAmmo{};
    bool weaponInfiniteGrenades{};
    bool weaponInfiniteDurability{};
    bool weaponNoRecoil{};
    bool weaponNoSpread{};
    float meleeAttackSpeedMultiplier{1.f};
    bool weaponOneHitKill{};
    bool instantSearch{};
    int addMoneyAmount{10000};
    int addCoinAmount{100};
    int addExperienceAmount{1000};
};

class GameReader {
public:
    ~GameReader();
    bool initialize();
    void shutdown();
    void update(int width, int height, const Features& features);
    const CameraState& camera() const { return camera_; }
    const std::vector<Target>& targets() const { return targets_; }
    const char* status() const { return status_; }
    uintptr_t localAbilityComponent() const { return localAbilityComponent_; }
    uintptr_t moduleBase() const { return module_; }
    bool teleportToMarker();
    void aim(const Features& features, HWND targetWindow, bool menuOpen = false);
    float aimRadiusPixels(int width, const Features& features) const;
private:
    uintptr_t module_{}, moduleSize_{};
    uintptr_t worldGlobal_{};
    uintptr_t namePool_{};
    uintptr_t controller_{};
    uintptr_t localPawn_{};
    uintptr_t lockedActor_{};
    float cameraTimestamp_{}, lastAimTimestamp_{-1.f};
    CameraState camera_{};
    std::vector<Target> targets_;
    char status_[96]{"等待 UE5 数据"};
    struct Offsets {
        uint32_t persistentLevel = 0x30;
        uint32_t owningGameInstance = 0x228;
        uint32_t localPlayers = 0x38;
        uint32_t playerController = 0x30;
        uint32_t acknowledgedPawn = 0x350;
        uint32_t cameraManager = 0x360;
        uint32_t cameraCache = 0x1530;
        uint32_t levelActors = 0xA0;
        uint32_t actorRoot = 0x1B8;
        uint32_t componentToWorld = 0x1E0;
        uint32_t mesh = 0x328;
        uint32_t skeletalMesh = 0x578;
        uint32_t boneTransforms = 0x928;
        uint32_t referenceBones = 0x360;
        uint32_t controlRotation{};
    } o_;
    struct SkeletonLayout {
        std::vector<int> parents;
        std::array<int, 4> aimIndices{}; // head, neck, spine, pelvis
        std::vector<std::pair<int, int>> edges;
        std::vector<int> boundsIndices;
    };
    std::unordered_map<uintptr_t, SkeletonLayout> skeletonLayouts_;
    std::unordered_map<uintptr_t, bool> enemyClasses_;
    struct HealthLayout {
        uint32_t component{}, attribute{}, data{}, current{}, maximum{}, dead{};
    };
    std::unordered_map<uintptr_t, HealthLayout> healthLayouts_;
    void readHealth(Target& target);
    struct PlayerAttributeOffsets {
        uint32_t tag{}, baseData{}, currentData{}, dead{};
    };
    std::unordered_map<uintptr_t, PlayerAttributeOffsets> playerAttributeOffsets_;
    uintptr_t localAbilityComponent_{};
    uint32_t abilityComponentOffset_{}, attributesOffset_{};
    uintptr_t playerAbilityComponent(uintptr_t pawn);
    void refillPlayerAttributes(uintptr_t component, const Features& features);
    struct PlayerComponent {
        uintptr_t object{}, owner{}, cls{};
        uint32_t objectIndex{}, ownerIndex{};
        bool operator==(const PlayerComponent&) const = default;
    };
    template <typename T> struct PlayerField {
        uint32_t offset{};
        T original{}, applied{};
        bool modified{};
    };
    struct PlayerSettings {
        PlayerComponent health{}, movement{};
        PlayerField<uint8_t> keepInventory{};
        PlayerField<float> jumpVelocity{};
        std::array<PlayerField<float>, 12> speedFields{};
    } playerSettings_;
    uintptr_t playerSettingsWorldClass_{}, playerSettingsPawnClass_{};
    uint32_t playerSettingsNetDriverOffset_{}, playerHealthOffset_{}, playerMovementOffset_{};
    PlayerComponent ownedPlayerComponent(uintptr_t pawn, uint32_t offset) const;
    bool isSamePlayerComponent(const PlayerComponent& component) const;
    template <typename T, typename Transform>
    void applyPlayerField(uintptr_t object, PlayerField<T>& field, Transform transform);
    template <typename T>
    void restorePlayerField(uintptr_t object, PlayerField<T>& field);
    void updatePlayerSettings(uintptr_t world, uintptr_t pawn, const Features& features);
    void restorePlayerSettings();
    struct PoseRefreshOverride {
        uintptr_t mesh{}, cls{};
        uint32_t nameIndex{}, number{}, offset{};
        uint8_t original{};
    };
    std::unordered_map<uintptr_t, PoseRefreshOverride> poseRefreshOverrides_;
    std::unordered_map<uintptr_t, uint32_t> poseRefreshOffsets_;
    void keepPoseFresh(uintptr_t mesh);
    void restorePoseRefresh(uintptr_t mesh);
    std::string name(uint32_t index) const;
    std::string className(uintptr_t object) const;
    uint32_t fieldOffset(uintptr_t structure, const char* field, uintptr_t* type = nullptr,
        uint32_t expectedSize = 0) const;
    bool isEnemy(uintptr_t actor);
    const SkeletonLayout* skeletonLayout(uintptr_t asset, int count);
    bool read(uintptr_t address, void* out, size_t size) const;
    bool writeLocal(uintptr_t address, const void* data, size_t size) const;
    template <typename T> T value(uintptr_t address) const { T v{}; read(address, &v, sizeof(v)); return v; }
    uintptr_t findWorldGlobal() const;
    bool project(Vec3 point, POINT& out, int width, int height) const;
    bool valid(uintptr_t p) const;
    bool validObject(uintptr_t p) const;
};
