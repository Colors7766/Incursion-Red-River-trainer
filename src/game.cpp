#include "game.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <psapi.h>
#include <algorithm>
#include <climits>
#include <type_traits>

namespace {
constexpr double pi = 3.14159265358979323846;
double rad(double v) { return v * pi / 180.0; }

Vec3 rotate(const double q[4], Vec3 v) {
    const Vec3 u{q[0], q[1], q[2]};
    const double s = q[3];
    const double dot = u.x * v.x + u.y * v.y + u.z * v.z;
    const double uu = u.x * u.x + u.y * u.y + u.z * u.z;
    const Vec3 cross{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    return {
        2.0 * dot * u.x + (s * s - uu) * v.x + 2.0 * s * cross.x,
        2.0 * dot * u.y + (s * s - uu) * v.y + 2.0 * s * cross.y,
        2.0 * dot * u.z + (s * s - uu) * v.z + 2.0 * s * cross.z
    };
}

BoneTransform combine(const BoneTransform& parent, const BoneTransform& child) {
    BoneTransform result{};
    const auto* a = parent.rotation;
    const auto* b = child.rotation;
    result.rotation[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    result.rotation[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    result.rotation[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    result.rotation[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    result.scale = {parent.scale.x * child.scale.x, parent.scale.y * child.scale.y, parent.scale.z * child.scale.z};
    const Vec3 scaled{parent.scale.x * child.translation.x, parent.scale.y * child.translation.y, parent.scale.z * child.translation.z};
    const Vec3 moved = rotate(parent.rotation, scaled);
    result.translation = {parent.translation.x + moved.x, parent.translation.y + moved.y, parent.translation.z + moved.z};
    return result;
}

bool finiteTransform(const BoneTransform& t) {
    double norm = 0.0;
    for (double q : t.rotation) {
        if (!std::isfinite(q)) return false;
        norm += q * q;
    }
    return norm > 0.8 && norm < 1.2 &&
        std::isfinite(t.translation.x) && std::isfinite(t.translation.y) && std::isfinite(t.translation.z) &&
        std::isfinite(t.scale.x) && std::isfinite(t.scale.y) && std::isfinite(t.scale.z);
}

void appendReaderLog(const char* text) {
    wchar_t path[MAX_PATH]{};
    const DWORD pathLength = GetTempPathW(MAX_PATH, path);
    if (!pathLength || pathLength >= MAX_PATH) return;
    wcscat_s(path, L"PQGameReader.log");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written{};
    WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    CloseHandle(file);
}
}

GameReader::~GameReader() {
    shutdown();
}

void GameReader::shutdown() {
    lockedActor_ = 0;
    localPawn_ = 0;
    safeWorld_ = 0;
    openedSafeBoxes_.clear();
    restorePlayerSettings();
    while (!poseRefreshOverrides_.empty()) restorePoseRefresh(poseRefreshOverrides_.begin()->first);
}

bool GameReader::teleportToMarker() {
    if (!worldGlobal_ || !validObject(localPawn_)) return false;
    const auto world = value<uintptr_t>(worldGlobal_);
    const auto level = validObject(world) ? value<uintptr_t>(world + o_.persistentLevel) : 0;
    if (!validObject(level)) return false;
    struct ActorArray { uintptr_t data; int32_t count, max; } actors{};
    if (!read(level + o_.levelActors, &actors, sizeof(actors)) || actors.count <= 0 ||
        actors.count > 65536 || actors.max < actors.count || !valid(actors.data)) return false;

    Vec3 destination{};
    bool found = false;
    for (int32_t i = 0; i < actors.count; ++i) {
        const auto actor = value<uintptr_t>(actors.data + uintptr_t(i) * sizeof(uintptr_t));
        if (!validObject(actor) || className(actor) != "BP_PingMarker_C") continue;
        const auto markerRoot = value<uintptr_t>(actor + o_.actorRoot);
        if (!validObject(markerRoot) || value<uintptr_t>(markerRoot + 0x20) != actor) continue;
        BoneTransform markerTransform{};
        if (!read(markerRoot + o_.componentToWorld, &markerTransform, sizeof(markerTransform)) ||
            !finiteTransform(markerTransform)) continue;
        destination = markerTransform.translation;
        found = true;
        break;
    }
    if (!found) return false;
    const auto pawnRoot = value<uintptr_t>(localPawn_ + o_.actorRoot);
    if (!validObject(pawnRoot) || value<uintptr_t>(pawnRoot + 0x20) != localPawn_) return false;
    // RelativeLocation and ComponentToWorld use UE5 FVector (three doubles).
    const bool relativeWritten = writeLocal(pawnRoot + 0x140, &destination, sizeof(destination));
    const bool worldWritten = writeLocal(pawnRoot + o_.componentToWorld + 0x20,
        &destination, sizeof(destination));
    return relativeWritten && worldWritten;
}

bool GameReader::writeLocal(uintptr_t address, const void* data, size_t size) const {
    if (!address || !data || !size || address > 0x7FFFFFFFFFFFULL - size) return false;
    __try { std::memcpy(reinterpret_cast<void*>(address), data, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void GameReader::keepPoseFresh(uintptr_t mesh) {
    // AlwaysTickPoseAndRefreshBones in the verified runtime enum.
    constexpr uint8_t alwaysRefresh = 0;
    if (auto existing = poseRefreshOverrides_.find(mesh); existing != poseRefreshOverrides_.end()) {
        const auto& saved = existing->second;
        if (value<uintptr_t>(mesh + 0x10) == saved.cls &&
            value<uint32_t>(mesh + 0x18) == saved.nameIndex &&
            value<uint32_t>(mesh + 0x1c) == saved.number) {
            uint8_t current{};
            if (read(mesh + saved.offset, &current, sizeof(current)) &&
                current > alwaysRefresh && current <= 4) {
                writeLocal(mesh + saved.offset, &alwaysRefresh, sizeof(alwaysRefresh));
            }
            return;
        }
        poseRefreshOverrides_.erase(existing);
    }
    const auto cls = value<uintptr_t>(mesh + 0x10);
    auto offset = poseRefreshOffsets_.find(cls);
    if (offset == poseRefreshOffsets_.end()) {
        const uint32_t field = fieldOffset(cls, "VisibilityBasedAnimTickOption");
        offset = poseRefreshOffsets_.emplace(cls, field).first;
    }
    if (!offset->second) return;
    uint8_t original{};
    if (!read(mesh + offset->second, &original, sizeof(original)) ||
        original == alwaysRefresh || original > 4) return;
    if (!writeLocal(mesh + offset->second, &alwaysRefresh, sizeof(alwaysRefresh))) return;
    poseRefreshOverrides_[mesh] = {mesh, cls, value<uint32_t>(mesh + 0x18),
        value<uint32_t>(mesh + 0x1c), offset->second, original};
    char line[160]{};
    std::snprintf(line, sizeof(line), "pose-refresh mesh=%p original=%u active=0\r\n",
        reinterpret_cast<void*>(mesh), unsigned(original));
    appendReaderLog(line);
}

void GameReader::restorePoseRefresh(uintptr_t mesh) {
    const auto found = poseRefreshOverrides_.find(mesh);
    if (found == poseRefreshOverrides_.end()) return;
    const auto saved = found->second;
    if (validObject(mesh) && value<uintptr_t>(mesh + 0x10) == saved.cls &&
        value<uint32_t>(mesh + 0x18) == saved.nameIndex &&
        value<uint32_t>(mesh + 0x1c) == saved.number &&
        value<uint8_t>(mesh + saved.offset) == 0) {
        writeLocal(mesh + saved.offset, &saved.original, sizeof(saved.original));
    }
    poseRefreshOverrides_.erase(found);
}

bool GameReader::valid(uintptr_t p) const {
    if (!p || p > 0x7FFFFFFFFFFFULL) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
    return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

bool GameReader::read(uintptr_t address, void* out, size_t size) const {
    if (!out || !size || !address || address > 0x7FFFFFFFFFFFULL - size) return false;
    __try { std::memcpy(out, reinterpret_cast<void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool GameReader::validObject(uintptr_t p) const {
    if (!p || p > 0x7FFFFFFFFFFFULL) return false;
    const auto vtable = value<uintptr_t>(p);
    return vtable >= module_ && vtable < module_ + moduleSize_;
}

std::string GameReader::name(uint32_t index) const {
    if (!namePool_ || (index >> 16) >= 8192) return {};
    auto block = value<uintptr_t>(namePool_ + 0x10 + uintptr_t(index >> 16) * 8);
    auto entry = block + uintptr_t(index & 0xffff) * 2;
    const auto header = value<uint16_t>(entry);
    const int length = header >> 6;
    if (length <= 0 || length > 1023) return {};
    if (header & 1) {
        std::wstring text(length, L'\0');
        if (!read(entry + 2, text.data(), size_t(length) * 2)) return {};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) return {};
        std::string result(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), bytes, nullptr, nullptr);
        return result;
    }
    std::string result(length, '\0');
    return read(entry + 2, result.data(), size_t(length)) ? result : std::string{};
}

std::string GameReader::className(uintptr_t object) const {
    auto cls = value<uintptr_t>(object + 0x10);
    return name(value<uint32_t>(cls + 0x18));
}

uint32_t GameReader::fieldOffset(uintptr_t structure, const char* field, uintptr_t* type,
    uint32_t expectedSize) const {
    for (int depth = 0; depth < 32 && validObject(structure); ++depth) {
        auto property = value<uintptr_t>(structure + 0x50);
        for (int i = 0; property && i < 4096; ++i) {
            if (name(value<uint32_t>(property + 0x20)) == field) {
                const auto offset = value<uint32_t>(property + 0x44);
                const auto elementSize = value<uint32_t>(property + 0x34);
                const auto structureSize = value<uint32_t>(structure + 0x58);
                if (!elementSize || (expectedSize && elementSize != expectedSize) ||
                    offset >= structureSize || elementSize > structureSize - offset) return 0;
                if (type) *type = value<uintptr_t>(property + 0x70);
                return offset;
            }
            property = value<uintptr_t>(property + 0x18);
        }
        structure = value<uintptr_t>(structure + 0x40);
    }
    return 0;
}

void GameReader::readHealth(Target& target) {
    const auto actorClass = value<uintptr_t>(target.actor + 0x10);
    auto found = healthLayouts_.find(actorClass);
    if (found == healthLayouts_.end()) {
        HealthLayout layout{};
        layout.component = fieldOffset(actorClass, "HealthComponent");
        if (!layout.component) return;
        const auto component = value<uintptr_t>(target.actor + layout.component);
        if (!validObject(component)) return;
        layout.attribute = fieldOffset(value<uintptr_t>(component + 0x10), "Health");
        if (!layout.attribute) return;
        const auto attribute = value<uintptr_t>(component + layout.attribute);
        if (!validObject(attribute)) return;
        uintptr_t dataType{};
        const auto attributeClass = value<uintptr_t>(attribute + 0x10);
        layout.dead = fieldOffset(attributeClass, "bIsDead");
        layout.data = fieldOffset(attributeClass, "CurrentData", &dataType);
        if (!layout.data || !validObject(dataType) ||
            name(value<uint32_t>(dataType + 0x18)) != "SimpleAttributeData") return;
        layout.current = fieldOffset(dataType, "BaseValue");
        layout.maximum = fieldOffset(dataType, "MaxValue");
        if (layout.current != 0 || layout.maximum != 8 ||
            fieldOffset(dataType, "MinValue") != 4 ||
            value<uint32_t>(dataType + 0x58) < 12) return;
        found = healthLayouts_.emplace(actorClass, layout).first;
    }
    const auto& layout = found->second;
    const auto component = value<uintptr_t>(target.actor + layout.component);
    if (!validObject(component)) return;
    const auto attribute = value<uintptr_t>(component + layout.attribute);
    if (!validObject(attribute)) return;
    if (layout.dead) target.dead = value<uint8_t>(attribute + layout.dead) != 0;
    float current{}, maximum{};
    if (!read(attribute + layout.data + layout.current, &current, sizeof(current)) ||
        !read(attribute + layout.data + layout.maximum, &maximum, sizeof(maximum)) ||
        !std::isfinite(current) || !std::isfinite(maximum) || maximum <= 0.f) return;
    target.health = std::clamp(current, 0.f, maximum);
    target.maxHealth = maximum;
    target.healthValid = true;
}

uintptr_t GameReader::playerAbilityComponent(uintptr_t pawn) {
    if (!validObject(pawn)) return 0;
    if (!abilityComponentOffset_)
        abilityComponentOffset_ = fieldOffset(value<uintptr_t>(pawn + 0x10), "GameplayAbilityComponent");
    if (!abilityComponentOffset_) return 0;
    const auto component = value<uintptr_t>(pawn + abilityComponentOffset_);
    return validObject(component) && value<uintptr_t>(component + 0x20) == pawn ? component : 0;
}

void GameReader::refillPlayerAttributes(uintptr_t component, const Features& features) {
    if (!component || (!features.infiniteHealth && !features.infiniteBodyStamina &&
        !features.infiniteArmStamina)) return;
    if (!attributesOffset_)
        attributesOffset_ = fieldOffset(value<uintptr_t>(component + 0x10), "Attributes");
    if (!attributesOffset_) return;

    struct AttributeArray { uintptr_t data; int32_t count, capacity; } attributes{};
    if (!read(component + attributesOffset_, &attributes, sizeof(attributes)) ||
        attributes.count <= 0 || attributes.count > 64 || attributes.capacity < attributes.count) return;
    std::array<uintptr_t, 64> entries{};
    if (!read(attributes.data, entries.data(), attributes.count * sizeof(uintptr_t))) return;

    struct AttributeData { float value, minimum, maximum; };
    static_assert(sizeof(AttributeData) == 12);
    for (int i = 0; i < attributes.count; ++i) {
        const auto attribute = entries[i];
        if (!validObject(attribute) || value<uintptr_t>(attribute + 0x20) != component) continue;
        const auto cls = value<uintptr_t>(attribute + 0x10);
        auto layout = playerAttributeOffsets_.find(cls);
        if (layout == playerAttributeOffsets_.end()) {
            PlayerAttributeOffsets offsets{};
            uintptr_t baseType{}, currentType{};
            offsets.tag = fieldOffset(cls, "AttributeTag");
            offsets.baseData = fieldOffset(cls, "BaseData", &baseType);
            offsets.currentData = fieldOffset(cls, "CurrentData", &currentType);
            offsets.dead = fieldOffset(cls, "bIsDead");
            if (!offsets.tag || !offsets.baseData || !offsets.currentData ||
                baseType != currentType || !validObject(currentType) ||
                name(value<uint32_t>(currentType + 0x18)) != "SimpleAttributeData" ||
                fieldOffset(currentType, "MinValue") != 4 || fieldOffset(currentType, "MaxValue") != 8) continue;
            layout = playerAttributeOffsets_.emplace(cls, offsets).first;
        }
        const auto& offsets = layout->second;
        const auto tag = name(value<uint32_t>(attribute + offsets.tag));
        if (tag == "Character.Attributes.Health") {
            uint8_t dead{};
            if (!features.infiniteHealth || !offsets.dead ||
                !read(attribute + offsets.dead, &dead, sizeof(dead)) || dead) continue;
        } else if (tag == "Character.Attributes.Stamina.Body") {
            if (!features.infiniteBodyStamina) continue;
        } else if (tag == "Character.Attributes.Stamina.Arm") {
            if (!features.infiniteArmStamina) continue;
        } else {
            continue;
        }
        for (const auto dataOffset : {offsets.baseData, offsets.currentData}) {
            AttributeData data{};
            if (read(attribute + dataOffset, &data, sizeof(data)) &&
                std::isfinite(data.value) && std::isfinite(data.minimum) &&
                std::isfinite(data.maximum) && data.maximum > data.minimum && data.value < data.maximum) {
                writeLocal(attribute + dataOffset, &data.maximum, sizeof(data.maximum));
            }
        }
    }
}

GameReader::PlayerComponent GameReader::ownedPlayerComponent(uintptr_t pawn, uint32_t offset) const {
    if (!offset) return {};
    const auto object = value<uintptr_t>(pawn + offset);
    if (!validObject(object) || value<uintptr_t>(object + 0x20) != pawn) return {};
    const auto cls = value<uintptr_t>(object + 0x10);
    if (!validObject(cls)) return {};
    return {object, pawn, cls, value<uint32_t>(object + 0xc), value<uint32_t>(pawn + 0xc)};
}

bool GameReader::isSamePlayerComponent(const PlayerComponent& component) const {
    return validObject(component.object) && validObject(component.owner) &&
        value<uintptr_t>(component.object + 0x10) == component.cls &&
        value<uintptr_t>(component.object + 0x20) == component.owner &&
        value<uint32_t>(component.object + 0xc) == component.objectIndex &&
        value<uint32_t>(component.owner + 0xc) == component.ownerIndex;
}

template <typename T, typename Transform>
void GameReader::applyPlayerField(uintptr_t object, PlayerField<T>& field, Transform transform) {
    T current{};
    if (!field.offset || !read(object + field.offset, &current, sizeof(current))) return;
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(current)) return;
    }
    // If the game updates a field, use that new value as the unmodified baseline.
    const T original = field.modified && current == field.applied ? field.original : current;
    const T desired = transform(original);
    if (desired != current && !writeLocal(object + field.offset, &desired, sizeof(desired))) return;
    field.original = original;
    field.applied = desired;
    field.modified = desired != original;
}

template <typename T>
void GameReader::restorePlayerField(uintptr_t object, PlayerField<T>& field) {
    T current{};
    if (object && field.modified && read(object + field.offset, &current, sizeof(current)) &&
        current == field.applied) {
        writeLocal(object + field.offset, &field.original, sizeof(field.original));
    }
    field.modified = false;
}

void GameReader::restorePlayerSettings() {
    auto& settings = playerSettings_;
    restorePlayerField(isSamePlayerComponent(settings.health) ? settings.health.object : 0,
        settings.keepInventory);
    const auto movement = isSamePlayerComponent(settings.movement) ? settings.movement.object : 0;
    restorePlayerField(movement, settings.jumpVelocity);
    for (auto& field : settings.speedFields) restorePlayerField(movement, field);
    settings = {};
}

void GameReader::updatePlayerSettings(uintptr_t world, uintptr_t pawn, const Features& features) {
    constexpr auto speedPropertyNames = std::to_array({
        "MaxWalkSpeed", "MaxWalkSpeedCrouched", "MaxAimSpeed", "MaxAimSneakSpeed",
        "MaxSprintSpeed", "MaxTacSprintSpeed", "MaxSprintSpeedCrouched",
        "MaxSneakSpeed", "MaxSneakSpeedCrouched", "MinimumMoveSpeed",
        "RuntimeSpeedLimit", "LocalRuntimeSpeedLimit"});
    static_assert(speedPropertyNames.size() == std::tuple_size_v<decltype(PlayerSettings::speedFields)>);
    const auto multiplier = [](float setting) {
        return std::isfinite(setting) ? std::clamp(setting, 0.5f, 5.f) : 1.f;
    };
    const float speed = multiplier(features.playerSpeedMultiplier);
    const float jumpHeight = multiplier(features.playerJumpHeightMultiplier);
    auto& settings = playerSettings_;
    if ((!features.keepInventoryOnDeath && speed == 1.f && jumpHeight == 1.f) || !validObject(pawn)) {
        if (settings.health.object || settings.movement.object) restorePlayerSettings();
        return;
    }

    const auto worldClass = value<uintptr_t>(world + 0x10);
    if (worldClass != playerSettingsWorldClass_) {
        playerSettingsWorldClass_ = worldClass;
        playerSettingsNetDriverOffset_ = fieldOffset(worldClass, "NetDriver", nullptr, sizeof(uintptr_t));
    }
    uintptr_t netDriver{};
    if (!playerSettingsNetDriverOffset_ ||
        !read(world + playerSettingsNetDriverOffset_, &netDriver, sizeof(netDriver)) || netDriver) {
        restorePlayerSettings();
        return;
    }

    const auto pawnClass = value<uintptr_t>(pawn + 0x10);
    if (pawnClass != playerSettingsPawnClass_) {
        playerSettingsPawnClass_ = pawnClass;
        playerHealthOffset_ = fieldOffset(pawnClass, "HealthComponent", nullptr, sizeof(uintptr_t));
        playerMovementOffset_ = fieldOffset(pawnClass, "CharacterMovement", nullptr, sizeof(uintptr_t));
    }
    const auto health = features.keepInventoryOnDeath ? ownedPlayerComponent(pawn, playerHealthOffset_) : PlayerComponent{};
    if (health != settings.health) {
        restorePlayerField(isSamePlayerComponent(settings.health) ? settings.health.object : 0,
            settings.keepInventory);
        settings.health = health;
        // The game's setter writes the whole byte at this verified offset.
        const auto offset = fieldOffset(health.cls, "bKeepInventoryOnDeath", nullptr, sizeof(uint8_t));
        settings.keepInventory = {offset == 0x1d6 ? offset : 0};
    }
    if (health.object) {
        applyPlayerField(health.object, settings.keepInventory,
            [](uint8_t base) -> uint8_t { return base == 0 ? 1 : base; });
    }

    const auto movement = speed != 1.f || jumpHeight != 1.f
        ? ownedPlayerComponent(pawn, playerMovementOffset_) : PlayerComponent{};
    if (movement != settings.movement) {
        const auto old = isSamePlayerComponent(settings.movement) ? settings.movement.object : 0;
        restorePlayerField(old, settings.jumpVelocity);
        for (auto& field : settings.speedFields) restorePlayerField(old, field);
        settings.movement = movement;
        settings.jumpVelocity = {fieldOffset(movement.cls, "JumpZVelocity", nullptr, sizeof(float))};
        for (size_t i = 0; i < speedPropertyNames.size(); ++i) {
            settings.speedFields[i] = {fieldOffset(movement.cls, speedPropertyNames[i], nullptr, sizeof(float))};
        }
    }
    if (!movement.object) return;
    const auto scale = [this, &movement](PlayerField<float>& field, float factor) {
        if (factor == 1.f) {
            restorePlayerField(movement.object, field);
            return;
        }
        applyPlayerField(movement.object, field, [factor](float base) {
            return std::isfinite(base) && base > 0.f && base < 100000.f ? base * factor : base;
        });
    };
    for (auto& field : settings.speedFields) scale(field, speed);
    // The engine calculates apex height from the square of the launch speed.
    scale(settings.jumpVelocity, std::sqrt(jumpHeight));
}

bool GameReader::isEnemy(uintptr_t actor) {
    const auto cls = value<uintptr_t>(actor + 0x10);
    if (auto found = enemyClasses_.find(cls); found != enemyClasses_.end()) return found->second;
    bool enemy = false;
    for (auto current = cls; validObject(current); current = value<uintptr_t>(current + 0x40)) {
        if (name(value<uint32_t>(current + 0x18)) == "IRRAIBaseCharacter") { enemy = true; break; }
    }
    enemyClasses_[cls] = enemy;
    return enemy;
}

const GameReader::SkeletonLayout* GameReader::skeletonLayout(uintptr_t asset, int count) {
    if (auto found = skeletonLayouts_.find(asset); found != skeletonLayouts_.end())
        return found->second.parents.size() == size_t(count) ? &found->second : nullptr;
    struct Array { uintptr_t data; int32_t count, max; } array{};
    struct BoneInfo { uint32_t name, number; int32_t parent; };
    if (!validObject(asset) || !read(asset + o_.referenceBones, &array, sizeof(array)) ||
        array.count != count || array.max < count || count <= 0 || count > 1024) return nullptr;
    std::vector<BoneInfo> info(count);
    if (!read(array.data, info.data(), info.size() * sizeof(BoneInfo))) return nullptr;
    SkeletonLayout layout{};
    std::unordered_map<std::string, int> indices;
    for (int i = 0; i < count; ++i) {
        if (info[i].parent < -1 || info[i].parent >= i || (i > 0 && info[i].parent == -1)) return nullptr;
        auto boneName = name(info[i].name);
        for (char& c : boneName) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (i == 0 && boneName != "root") return nullptr;
        indices[boneName] = i;
        layout.parents.push_back(info[i].parent);
    }
    const char* aimNames[] = {"head", "neck_02", "spine_03", "pelvis"};
    for (int i = 0; i < 4; ++i) {
        const auto found = indices.find(aimNames[i]);
        if (found == indices.end()) return nullptr;
        layout.aimIndices[i] = found->second;
    }
    const char* joints[] = {"pelvis", "spine_01", "spine_02", "spine_03", "spine_04", "spine_05",
        "neck_01", "neck_02", "head", "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l",
        "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r", "thigh_l", "calf_l", "foot_l",
        "thigh_r", "calf_r", "foot_r"};
    for (const auto joint : joints) {
        auto found = indices.find(joint);
        if (found == indices.end()) continue;
        const int child = found->second;
        layout.boundsIndices.push_back(child);
        // The rig root is a ground anchor, so omit its connection to the pelvis.
        if (info[child].parent > 0) layout.edges.emplace_back(info[child].parent, child);
    }
    auto [it, inserted] = skeletonLayouts_.emplace(asset, std::move(layout));
    return &it->second;
}

uintptr_t GameReader::findWorldGlobal() const {
    // Observed RVA for this installed executable; object identities guard this binding.
    const auto slot = module_ + 0xC2C6B98;
    const auto world = value<uintptr_t>(slot);
    if (className(world) != "World") return 0;
    const auto level = value<uintptr_t>(world + o_.persistentLevel);
    const auto gi = value<uintptr_t>(world + o_.owningGameInstance);
    const auto localPlayer = value<uintptr_t>(value<uintptr_t>(gi + o_.localPlayers));
    const auto controller = value<uintptr_t>(localPlayer + o_.playerController);
    if (className(level) != "Level" || className(localPlayer) != "LocalPlayer" || !validObject(controller)) return 0;
    return slot;
}

bool GameReader::initialize() {
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &info, sizeof(info))) return false;
    module_ = uintptr_t(info.lpBaseOfDll); moduleSize_ = info.SizeOfImage;
    namePool_ = module_ + 0xC050E00;
    if (name(0) != "None") { std::strcpy(status_, "游戏版本与已验证数据不符"); return false; }
    worldGlobal_ = findWorldGlobal();
    if (!worldGlobal_) { std::strcpy(status_, "等待当前 World"); return false; }
    auto world = value<uintptr_t>(worldGlobal_);
    auto gi = value<uintptr_t>(world + o_.owningGameInstance);
    auto lp = value<uintptr_t>(value<uintptr_t>(gi + o_.localPlayers));
    auto pc = value<uintptr_t>(lp + o_.playerController);
    auto pcClass = value<uintptr_t>(pc + 0x10);
    o_.controlRotation = fieldOffset(pcClass, "ControlRotation");
    o_.acknowledgedPawn = fieldOffset(pcClass, "AcknowledgedPawn");
    o_.cameraManager = fieldOffset(pcClass, "PlayerCameraManager");
    auto cm = value<uintptr_t>(pc + o_.cameraManager);
    uintptr_t cacheType{}, povType{};
    o_.cameraCache = fieldOffset(value<uintptr_t>(cm + 0x10), "CameraCachePrivate", &cacheType);
    if (fieldOffset(cacheType, "POV", &povType) != 0x10 ||
        fieldOffset(povType, "Rotation") != 0x18 || fieldOffset(povType, "FOV") != 0x30) return false;
    auto pawn = value<uintptr_t>(pc + o_.acknowledgedPawn);
    o_.mesh = fieldOffset(value<uintptr_t>(pawn + 0x10), "Mesh");
    if (!o_.acknowledgedPawn || !o_.cameraManager || !o_.cameraCache || !o_.mesh || !o_.controlRotation) return false;
    auto mesh = value<uintptr_t>(pawn + o_.mesh);
    o_.skeletalMesh = fieldOffset(value<uintptr_t>(mesh + 0x10), "SkeletalMesh");
    if (!o_.skeletalMesh) return false;
    char line[256]{};
    std::snprintf(line, sizeof(line), "runtime-v2 PC=%p CM=%p pawn=%p Mesh=0x%X cameraCache=0x%X\r\n",
        reinterpret_cast<void*>(pc), reinterpret_cast<void*>(cm), reinterpret_cast<void*>(pawn), o_.mesh, o_.cameraCache);
    appendReaderLog(line);
    std::strcpy(status_, "运行时对象类型已核实");
    return true;
}

bool GameReader::project(Vec3 p, POINT& out, int width, int height) const {
    if (!camera_.valid) return false;
    const double pitch = rad(camera_.rotation.pitch), yaw = rad(camera_.rotation.yaw), roll = rad(camera_.rotation.roll);
    const double SP = std::sin(pitch), CP = std::cos(pitch), SY = std::sin(yaw), CY = std::cos(yaw);
    const Vec3 d{p.x - camera_.location.x, p.y - camera_.location.y, p.z - camera_.location.z};
    const double SR = std::sin(roll), CR = std::cos(roll);
    const double rightX = -SY * CR - SP * CY * SR;
    const double rightY =  CY * CR - SP * SY * SR;
    const double rightZ =  CP * SR;
    const double upX = SY * SR - SP * CY * CR;
    const double upY = -CY * SR - SP * SY * CR;
    const double upZ = CP * CR;
    const double forwardX = CP * CY;
    const double forwardY = CP * SY;
    const double forwardZ = SP;
    const double tx = rightX * d.x + rightY * d.y + rightZ * d.z;
    const double ty = upX * d.x + upY * d.y + upZ * d.z;
    const double tz = forwardX * d.x + forwardY * d.y + forwardZ * d.z;
    if (tz < 1.0) return false;
    const double focal = width / (2.0 * std::tan(rad(camera_.fov) * .5));
    out.x = LONG(width * .5 + tx * focal / tz);
    out.y = LONG(height * .5 - ty * focal / tz);
    return out.x >= -200 && out.x <= width + 200 && out.y >= -200 && out.y <= height + 200;
}

void GameReader::update(int width, int height, const Features& features) {
    static char lastReportedStatus[96]{};
    auto report = [&](const char* text) {
        strncpy_s(status_, sizeof(status_), text, _TRUNCATE);
        if (std::strcmp(lastReportedStatus, text) != 0) {
            char line[160]{};
            std::snprintf(line, sizeof(line), "%s\r\n", text);
            appendReaderLog(line);
            strncpy_s(lastReportedStatus, sizeof(lastReportedStatus), text, _TRUNCATE);
        }
    };
    auto noPlayer = [&](const char* text) {
        localPawn_ = 0;
        localAbilityComponent_ = 0;
        if (playerSettings_.health.object || playerSettings_.movement.object) restorePlayerSettings();
        report(text);
    };
    targets_.clear();
    safeBoxes_.clear();
    camera_.valid = false;
    controller_ = 0;
    localAbilityComponent_ = 0;
    if (!worldGlobal_) worldGlobal_ = findWorldGlobal();
    if (!worldGlobal_) { noPlayer("world_global=missing"); return; }
    auto world = value<uintptr_t>(worldGlobal_);
    if (!validObject(world)) { worldGlobal_ = 0; noPlayer("world=invalid"); return; }
    if (safeWorld_ != world) {
        safeWorld_ = world;
        openedSafeBoxes_.clear();
    }
    auto gameInstance = value<uintptr_t>(world + o_.owningGameInstance);
    if (!validObject(gameInstance)) { noPlayer("game_instance=invalid"); return; }
    struct TArray { uintptr_t data; int32_t count; int32_t max; };
    TArray localPlayers{};
    if (!read(gameInstance + o_.localPlayers, &localPlayers, sizeof(localPlayers)) || localPlayers.count <= 0 ||
        localPlayers.count > 8 || localPlayers.max < localPlayers.count || !valid(localPlayers.data)) {
        noPlayer("local_players=invalid"); return;
    }
    auto localPlayer = value<uintptr_t>(localPlayers.data);
    if (!validObject(localPlayer)) { noPlayer("local_player=invalid"); return; }
    auto controller = value<uintptr_t>(localPlayer + o_.playerController);
    if (!validObject(controller)) { noPlayer("player_controller=invalid"); return; }
    const auto localPawn = value<uintptr_t>(controller + o_.acknowledgedPawn);
    localPawn_ = localPawn;
    updatePlayerSettings(world, localPawn, features);
    if (features.infiniteHealth || features.infiniteBodyStamina ||
        features.infiniteArmStamina || features.preventBleeding || features.preventFracture) {
        localAbilityComponent_ = playerAbilityComponent(localPawn);
        refillPlayerAttributes(localAbilityComponent_, features);
    }
    auto cameraManager = value<uintptr_t>(controller + o_.cameraManager);
    if (!validObject(cameraManager)) { report("camera_manager=invalid"); return; }
    controller_ = controller;
    cameraTimestamp_ = value<float>(cameraManager + o_.cameraCache);
    bool cameraRead = false;
    {
        struct POV { Vec3 location; Rotator rotation; float fov; } pov{};
        if (read(cameraManager + o_.cameraCache + 0x10, &pov, sizeof(pov))) {
            camera_.location = pov.location;
            camera_.rotation = pov.rotation;
            camera_.fov = pov.fov;
            camera_.valid = std::isfinite(pov.location.x) && std::isfinite(pov.location.y) && std::isfinite(pov.location.z) &&
                std::isfinite(pov.rotation.pitch) && std::isfinite(pov.rotation.yaw) && std::isfinite(pov.rotation.roll) &&
                std::isfinite(pov.fov) && pov.fov > 1.f && pov.fov < 180.f;
            cameraRead = camera_.valid;
        }
    }
    if (!cameraRead) camera_.valid = false;
    auto level = value<uintptr_t>(world + o_.persistentLevel);
    if (!validObject(level)) { report("persistent_level=invalid"); return; }
    TArray actors{};
    if (!read(level + o_.levelActors, &actors, sizeof(actors))) { report("actor_array=unreadable"); return; }
    if (actors.count <= 0 || actors.count > 65536 || actors.max < actors.count || !valid(actors.data)) {
        char state[96]{};
        std::snprintf(state, sizeof(state), "actor_array=invalid count=%d max=%d", actors.count, actors.max);
        report(state);
        return;
    }
    if (!camera_.valid) { report("camera=invalid"); return; }
    if (features.espSafeBoxes) {
        for (int32_t i = 0; i < actors.count; ++i) {
            const auto actor = value<uintptr_t>(actors.data + uintptr_t(i) * sizeof(uintptr_t));
            if (!validObject(actor)) continue;
            const auto type = className(actor);
            const bool large = type == "BP_LootContainer_Safe_02_C";
            const bool small = type == "BP_LootContainer_Safe_01_C";
            if (!large && !small) continue;
            const auto interaction = value<uintptr_t>(actor + 0x2B8);
            if (!validObject(interaction) || value<uint8_t>(interaction + 0x139) == 0) continue;
            // Option 0 requires the key; option 1 is the no-key interaction used
            // after the safe has been unlocked. Cache that transition because
            // ActiveInteractionOption is only populated while the target updates.
            TArray options{};
            const auto active = value<uintptr_t>(interaction + 0x188);
            if (read(interaction + 0x160, &options, sizeof(options)) &&
                options.count > 1 && options.count <= 16 && options.max >= options.count &&
                valid(options.data) && validObject(active)) {
                for (int32_t optionIndex = 0; optionIndex < options.count; ++optionIndex) {
                    if (value<uintptr_t>(options.data + uintptr_t(optionIndex) * sizeof(uintptr_t)) == active) {
                        if (optionIndex == 1) openedSafeBoxes_.insert(actor);
                        break;
                    }
                }
            }
            if (openedSafeBoxes_.contains(actor)) continue;
            const auto root = value<uintptr_t>(actor + o_.actorRoot);
            if (!validObject(root) || value<uintptr_t>(root + 0x20) != actor) continue;
            BoneTransform transform{};
            if (!read(root + o_.componentToWorld, &transform, sizeof(transform)) ||
                !finiteTransform(transform)) continue;
            const auto dx = transform.translation.x - camera_.location.x;
            const auto dy = transform.translation.y - camera_.location.y;
            const auto dz = transform.translation.z - camera_.location.z;
            const float distance = float(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0);
            if (distance > features.safeBoxDistance) continue;
            SafeBoxTarget safe{};
            safe.location = transform.translation;
            safe.distance = distance;
            safe.large = large;
            safe.visible = project(safe.location, safe.screen, width, height);
            if (safe.visible) safeBoxes_.push_back(safe);
        }
    }
    int enemies = 0, skeletons = 0, boxes = 0, lines = 0;
    std::vector<uintptr_t> activeMeshes;
    float nearest = 100000.f;
    const float livingDistance = features.aimbot ? (std::max)(features.espDistance, features.aimDistance) : features.espDistance;
    const float readDistance = features.espCorpse ? (std::max)(livingDistance, features.corpseDistance) : livingDistance;
    for (int32_t i = 0; i < actors.count; ++i) {
        auto actor = value<uintptr_t>(actors.data + uintptr_t(i) * sizeof(uintptr_t));
        if (!validObject(actor) || actor == localPawn || !isEnemy(actor)) continue;
        ++enemies;
        auto root = value<uintptr_t>(actor + o_.actorRoot);
        if (!validObject(root) || value<uintptr_t>(root + 0x20) != actor) continue;
        BoneTransform rootTransform{};
        if (!read(root + o_.componentToWorld, &rootTransform, sizeof(rootTransform)) || !finiteTransform(rootTransform)) continue;
        const Vec3 pos = rootTransform.translation;
        const double dx = pos.x - camera_.location.x, dy = pos.y - camera_.location.y, dz = pos.z - camera_.location.z;
        const float distanceMeters = float(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0);
        if (distanceMeters > readDistance) continue;
        Target t{};
        t.actor = actor;
        t.root = pos;
        t.distance = distanceMeters;
        readHealth(t);

        auto mesh = value<uintptr_t>(actor + o_.mesh);
        const bool hasMesh = validObject(mesh) && value<uintptr_t>(mesh + 0x20) == actor;
        if (t.dead) {
            if (features.espCorpse && distanceMeters <= features.corpseDistance) {
                BoneTransform corpseTransform{};
                if (hasMesh && read(mesh + o_.componentToWorld, &corpseTransform, sizeof(corpseTransform)) &&
                    finiteTransform(corpseTransform)) t.root = corpseTransform.translation;
                t.visible = project(t.root, t.rootScreen, width, height);
                targets_.push_back(std::move(t));
            }
            continue;
        }
        if (distanceMeters > livingDistance || !hasMesh) continue;
        activeMeshes.push_back(mesh);
        if (features.espSkeleton || features.aimbot) keepPoseFresh(mesh);
        BoneTransform meshTransform{};
        if (!read(mesh + o_.componentToWorld, &meshTransform, sizeof(meshTransform)) || !finiteTransform(meshTransform)) continue;
        TArray boneArray{};
        if (!read(mesh + o_.boneTransforms, &boneArray, sizeof(boneArray)) || boneArray.count <= 0 ||
            boneArray.count > 1024 || boneArray.max < boneArray.count || boneArray.max > 2048) continue;
        auto asset = value<uintptr_t>(mesh + o_.skeletalMesh);
        const auto* layout = skeletonLayout(asset, boneArray.count);
        if (!layout) continue;
        std::vector<BoneTransform> componentBones(boneArray.count);
        if (!read(boneArray.data, componentBones.data(), componentBones.size() * sizeof(BoneTransform))) continue;
        bool poseValid = true;
        for (int bone = 0; bone < boneArray.count; ++bone) {
            if (!finiteTransform(componentBones[bone])) { poseValid = false; break; }
            const auto parent = layout->parents[bone];
            if (parent >= 0) componentBones[bone] = combine(componentBones[parent], componentBones[bone]);
        }
        if (!poseValid) continue;
        t.meshOrigin = meshTransform.translation;
        t.boneCount = boneArray.count;
        ++skeletons;
        nearest = (std::min)(nearest, distanceMeters);
        project(pos, t.rootScreen, width, height);
        static constexpr int aimSlots[] = {9, 8, 4, 1};
        for (int j = 0; j < 4; ++j) {
            const auto worldBone = combine(meshTransform, componentBones[layout->aimIndices[j]]);
            t.aimWorld[aimSlots[j]] = worldBone.translation;
            t.aimValid[aimSlots[j]] = project(worldBone.translation, t.aimScreen[aimSlots[j]], width, height);
            if (j == 0) { t.head = worldBone.translation; t.headScreen = t.aimScreen[9]; }
        }
        std::vector<POINT> projected(boneArray.count);
        std::vector<bool> projectedValid(boneArray.count);
        for (const auto index : layout->boundsIndices) {
            const auto worldBone = combine(meshTransform, componentBones[index]);
            projectedValid[index] = project(worldBone.translation, projected[index], width, height);
        }
        LONG minX = LONG_MAX, minY = LONG_MAX, maxX = LONG_MIN, maxY = LONG_MIN;
        for (const auto index : layout->boundsIndices) {
            if (!projectedValid[index]) continue;
            minX = (std::min)(minX, projected[index].x); maxX = (std::max)(maxX, projected[index].x);
            minY = (std::min)(minY, projected[index].y); maxY = (std::max)(maxY, projected[index].y);
        }
        for (const auto& edge : layout->edges) {
            POINT parent{};
            bool parentValid = projectedValid[edge.first];
            if (parentValid) parent = projected[edge.first];
            else parentValid = project(combine(meshTransform, componentBones[edge.first]).translation, parent, width, height);
            if (parentValid && projectedValid[edge.second])
                t.skeletonLines.emplace_back(parent, projected[edge.second]);
        }
        if (t.aimValid[9] && maxY > minY && maxX > minX) {
            const LONG pad = (std::max)(3L, (maxY - minY) / 20);
            t.boxMin = {minX - pad, minY - pad}; t.boxMax = {maxX + pad, maxY + pad};
            t.boxValid = true; ++boxes;
        }
        lines += int(t.skeletonLines.size());
        t.visible = t.aimValid[9];
        targets_.push_back(std::move(t));
    }
    std::vector<uintptr_t> restoreMeshes;
    for (const auto& [mesh, saved] : poseRefreshOverrides_)
        if ((!features.espSkeleton && !features.aimbot) ||
            std::find(activeMeshes.begin(), activeMeshes.end(), mesh) == activeMeshes.end()) restoreMeshes.push_back(mesh);
    for (const auto mesh : restoreMeshes) restorePoseRefresh(mesh);

    char state[96]{};
    std::snprintf(state, sizeof(state), "AI=%d poses=%d boxes=%d lines=%d near=%.1fm camera=ok", enemies, skeletons, boxes, lines, nearest);
    report(state);
    static ULONGLONG lastDetail{};
    if (GetTickCount64() - lastDetail >= 2000) {
        lastDetail = GetTickCount64();
        char line[512]{};
        const auto* t = targets_.empty() ? nullptr : &*std::min_element(targets_.begin(), targets_.end(),
            [](const Target& a, const Target& b) { return a.distance < b.distance; });
        std::snprintf(line, sizeof(line), "runtime-v2 tick=%llu size=%dx%d cam=(%.2f,%.2f,%.2f) rot=(%.2f,%.2f,%.2f) fov=%.2f %s nearestActor=%p bones=%d head=(%ld,%ld) skeletonSegments=%zu\r\n",
            GetTickCount64(), width, height, camera_.location.x, camera_.location.y, camera_.location.z,
            camera_.rotation.pitch, camera_.rotation.yaw, camera_.rotation.roll, camera_.fov, state,
            reinterpret_cast<void*>(t ? t->actor : 0), t ? t->boneCount : 0,
            t ? t->headScreen.x : 0L, t ? t->headScreen.y : 0L, t ? t->skeletonLines.size() : 0);
        appendReaderLog(line);
    }
}

float GameReader::aimRadiusPixels(int width, const Features& features) const {
    if (!camera_.valid || width <= 0 || !std::isfinite(features.aimFov)) return 0.f;
    constexpr double referenceCameraFov = 90.0;
    const double focal = width * .5 / std::tan(rad(referenceCameraFov) * .5);
    return float(focal * std::tan(rad(features.aimFov) * .5));
}

void GameReader::aim(const Features& features, HWND targetWindow, bool menuOpen) {
    const auto releaseLock = [this] { lockedActor_ = 0; lastAimTimestamp_ = -1.f; };
    if (!features.aimbot || menuOpen || features.captureAimKey || !camera_.valid ||
        !validObject(controller_) || GetForegroundWindow() != targetWindow ||
        !(GetAsyncKeyState(features.aimKey) & 0x8000)) {
        releaseLock(); return;
    }
    RECT rc{};
    if (!GetClientRect(targetWindow, &rc) || rc.right <= 0 || rc.bottom <= 0) { releaseLock(); return; }
    if (features.aimBone < 0 || features.aimBone >= 10) { releaseLock(); return; }
    const POINT origin{rc.right / 2, rc.bottom / 2};
    const float acquireRadius = aimRadiusPixels(rc.right, features);
    Target* selected = nullptr;
    if (lockedActor_) {
        const auto locked = std::find_if(targets_.begin(), targets_.end(),
            [this](const Target& target) { return target.actor == lockedActor_; });
        if (locked != targets_.end()) {
            if (locked->dead) releaseLock();
            else selected = &*locked;
        } else if (!validObject(lockedActor_) || !isEnemy(lockedActor_)) {
            releaseLock();
        } else {
            Target lockState{};
            lockState.actor = lockedActor_;
            readHealth(lockState);
            if (lockState.dead) releaseLock();
            else return;
        }
    }
    if (!lockedActor_) {
        float closest = acquireRadius;
        for (auto& t : targets_) {
            if (t.dead || t.distance > features.aimDistance || !t.aimValid[features.aimBone]) continue;
            const auto point = t.aimScreen[features.aimBone];
            if (point.x < 0 || point.y < 0 || point.x >= rc.right || point.y >= rc.bottom) continue;
            const float dx = float(point.x - origin.x), dy = float(point.y - origin.y);
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance < closest) { closest = distance; selected = &t; }
        }
    }
    if (!selected) return;
    if (!selected->aimValid[features.aimBone]) return;
    const auto aimPoint = selected->aimScreen[features.aimBone];
    if (aimPoint.x < 0 || aimPoint.y < 0 || aimPoint.x >= rc.right || aimPoint.y >= rc.bottom) return;
    const bool acquired = lockedActor_ != selected->actor;
    lockedActor_ = selected->actor;
    // One correction per observed game camera update; never accumulate against stale feedback.
    if (lastAimTimestamp_ == cameraTimestamp_) return;
    const auto position = selected->aimWorld[features.aimBone];
    const double dx = position.x - camera_.location.x, dy = position.y - camera_.location.y,
        dz = position.z - camera_.location.z;
    const double flat = std::hypot(dx, dy);
    if (!std::isfinite(flat) || flat < .001 || !std::isfinite(dz)) return;
    const auto normalize = [](double angle) { return std::remainder(angle, 360.0); };
    const double desiredPitch = std::atan2(dz, flat) * 180.0 / pi;
    const double desiredYaw = std::atan2(dy, dx) * 180.0 / pi;
    const double pitchError = normalize(desiredPitch - camera_.rotation.pitch);
    const double yawError = normalize(desiredYaw - camera_.rotation.yaw);
    Rotator control{};
    if (!read(controller_ + o_.controlRotation, &control, sizeof(control)) ||
        !std::isfinite(control.pitch) || !std::isfinite(control.yaw) || !std::isfinite(control.roll)) return;
    // Preserve the measured camera/control offset, including the game's current camera effects.
    Rotator corrected = control;
    corrected.pitch = std::clamp(normalize(control.pitch) + pitchError, -89.0, 89.0);
    corrected.yaw = normalize(control.yaw + yawError);
    if (!writeLocal(controller_ + o_.controlRotation, &corrected, sizeof(corrected))) return;
    lastAimTimestamp_ = cameraTimestamp_;
    static ULONGLONG lastReport{};
    if (acquired || GetTickCount64() - lastReport >= 1000) {
        lastReport = GetTickCount64();
        char line[224]{};
        std::snprintf(line, sizeof(line), "aim-lock actor=%p bone=%d distance=%.2fm error=(%.4f,%.4f) cameraTime=%.4f\r\n",
            reinterpret_cast<void*>(selected->actor), features.aimBone, selected->distance,
            pitchError, yawError, cameraTimestamp_);
        appendReaderLog(line);
    }
}
