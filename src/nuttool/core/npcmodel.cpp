#include "npcmodel.hpp"

#include "animkeys.hpp"
#include "assetdb.hpp"
#include "caff.hpp"
#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace nut::npcmodel {
namespace {

using blockmodel::detail::Sym;

constexpr double kPi = 3.14159265358979323846;
constexpr float kFps = 30;
constexpr float kDefaultSeconds = 2.4f;

// An animation's symbols (1-based): the animation, then its string pool (the bone names).
constexpr uint32_t kData = 1, kPool = 2;

float smooth(float e0, float e1, float x) {
    if (!(e1 > e0)) return x >= e1 ? 1.0f : 0.0f;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

std::vector<blockmodel::SkinBone> skinBones(const Rig& rig) {
    std::vector<blockmodel::SkinBone> out;
    for (const Bone& b : rig.bones) out.push_back({b.name, b.parent, b.world});
    blockmodel::detail::boneDepths(out);
    if (out.empty() || out.size() > 255) fail("a rig has 1 to 255 bones");
    return out;
}

int boneIndex(const Rig& rig, const std::string& name) {
    for (size_t i = 0; i < rig.bones.size(); ++i)
        if (rig.bones[i].name == name) return int(i);
    return -1;
}

// x y z w from XYZ Euler degrees, X turning first (as animkeys reads "rot").
std::array<float, 4> euler(double x, double y, double z) {
    const double deg[3] = {x, y, z};
    double r[4] = {0, 0, 0, 1};
    for (int k = 0; k < 3; ++k) {
        const double h = deg[k] * kPi / 360, s = std::sin(h), c = std::cos(h);
        double q[4] = {0, 0, 0, c};
        q[k] = s;
        const double m[4] = {q[3] * r[0] + q[0] * r[3] + q[1] * r[2] - q[2] * r[1], q[3] * r[1] - q[0] * r[2] + q[1] * r[3] + q[2] * r[0],
                             q[3] * r[2] + q[0] * r[1] - q[1] * r[0] + q[2] * r[3], q[3] * r[3] - q[0] * r[0] - q[1] * r[1] - q[2] * r[2]};
        std::copy(m, m + 4, r);
    }
    return {float(r[0]), float(r[1]), float(r[2]), float(r[3])};
}

// "breathe": the hips swaying, the chest rising with each breath, the head looking around and nodding,
// the arms easing out and in; every motion a whole number of cycles a loop.
std::vector<model::Key> breathe(const Rig& rig, uint32_t frames, float duration) {
    const size_t n = rig.bones.size();
    std::vector<model::Key> keys(size_t(frames) * n);
    const int hips = boneIndex(rig, "BACKA"), chest = boneIndex(rig, "BACKB"), neck = boneIndex(rig, "NECK1"), head = boneIndex(rig, "HEAD");
    const int left = boneIndex(rig, "LF_S"), right = boneIndex(rig, "RT_S");
    for (uint32_t f = 0; f < frames; ++f) {
        const double a = 2 * kPi * (double(f) / kFps) / duration;
        auto set = [&](int bone, const std::array<float, 4>& q) {
            if (bone >= 0) std::copy(q.begin(), q.end(), keys[size_t(f) * n + size_t(bone)].rotation);
        };
        set(hips, euler(0, 3.0 * std::sin(a + 1.1), 4.0 * std::sin(a + 0.4)));
        set(chest, euler(5.0 * std::sin(2 * a), 0, -2.0 * std::sin(a + 0.4)));
        set(neck, euler(3.0 * std::sin(2 * a + 2.0), 6.0 * std::sin(a + 0.9), 0));
        set(head, euler(5.0 * std::sin(2 * a + 0.5), 12.0 * std::sin(a + 1.6), 3.0 * std::sin(a)));
        const double arms = 5.0 + 4.0 * std::sin(2 * a + 0.3);
        set(left, euler(0, 0, arms));
        set(right, euler(0, 0, -arms));
    }
    return keys;
}

// "walk": a stride a loop, each leg swinging through and its knee bending as it comes forward, the arms
// swinging against the legs with the elbows a little bent, the hips and chest twisting against each other.
std::vector<model::Key> walk(const Rig& rig, uint32_t frames, float duration) {
    const size_t n = rig.bones.size();
    std::vector<model::Key> keys(size_t(frames) * n);
    const int hips = boneIndex(rig, "BACKA"), chest = boneIndex(rig, "BACKB"), head = boneIndex(rig, "HEAD");
    const int left = boneIndex(rig, "LF_S"), right = boneIndex(rig, "RT_S"), leftElbow = boneIndex(rig, "LF_E"), rightElbow = boneIndex(rig, "RT_E");
    const int leftHip = boneIndex(rig, "LF_H"), rightHip = boneIndex(rig, "RT_H"), leftKnee = boneIndex(rig, "LF_K"), rightKnee = boneIndex(rig, "RT_K");
    const int leftFoot = boneIndex(rig, "LF_F"), rightFoot = boneIndex(rig, "RT_F");
    for (uint32_t f = 0; f < frames; ++f) {
        const double a = 2 * kPi * (double(f) / kFps) / duration;
        model::Key* row = &keys[size_t(f) * n];
        auto set = [&](int bone, const std::array<float, 4>& q) {
            if (bone >= 0) std::copy(q.begin(), q.end(), row[size_t(bone)].rotation);
        };
        // A turn about x of -s swings a leg's foot forward (+z); its knee bends (+) as it comes through.
        const double s = std::sin(a);
        set(leftHip, euler(-30.0 * s, 0, 0));
        set(rightHip, euler(30.0 * s, 0, 0));
        set(leftKnee, euler(40.0 * std::max(0.0, std::sin(a + kPi / 2)), 0, 0));
        set(rightKnee, euler(40.0 * std::max(0.0, std::sin(a - kPi / 2)), 0, 0));
        set(leftFoot, euler(-10.0 * s, 0, 0));
        set(rightFoot, euler(10.0 * s, 0, 0));
        set(hips, euler(2.0 * std::sin(2 * a), 6.0 * s, 0));
        set(chest, euler(4.0, -9.0 * s, 0));
        set(head, euler(0, 3.0 * s, 0));
        set(left, euler(25.0 * s, 0, 4.0));
        set(right, euler(-25.0 * s, 0, -4.0));
        set(leftElbow, euler(-15.0 - 10.0 * std::max(0.0, s), 0, 0));
        set(rightElbow, euler(-15.0 - 10.0 * std::max(0.0, -s), 0, 0));
    }
    return keys;
}

// A CAFF holding one asset with no pointers: a .data symbol named `name`, of `kind`.
Bytes oneAsset(const std::string& name, uint8_t kind, const Bytes& data) {
    caff::Caff c;
    c.symbolNames.push_back(name);
    caff::Section s;
    s.name = ".data";
    s.alignLog2 = 4;
    c.sections.push_back(s);
    c.appendSymbol(1, 1, kind, data);
    while (c.sections[0].data.size() % 16) c.sections[0].data.push_back(0);
    return c.serialize();
}

float fromBits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

} // namespace

Rig autoRig(const std::vector<std::array<float, 3>>& positions) {
    if (positions.empty()) fail("autoRig: the mesh has no vertices");
    std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const auto& p : positions)
        for (size_t k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
    const float h = hi[1] - lo[1];
    if (!(h > 0)) fail("autoRig: the mesh is flat");
    const float cx = (lo[0] + hi[0]) / 2, cz = (lo[2] + hi[2]) / 2;
    // The middle (x, z) of what lies between two heights (fractions of the height), else the bounds'.
    auto middle = [&](float from, float to) {
        double x = 0, z = 0;
        size_t count = 0;
        for (const auto& p : positions)
            if (p[1] >= lo[1] + from * h && p[1] <= lo[1] + to * h) x += p[0], z += p[2], ++count;
        return count ? std::array<float, 2>{float(x / double(count)), float(z / double(count))} : std::array<float, 2>{cx, cz};
    };
    auto at = [&](const std::array<float, 2>& m, float height) { return std::array<float, 3>{m[0], lo[1] + height * h, m[1]}; };
    const auto hips = middle(0.45f, 0.55f), chest = middle(0.63f, 0.73f), neck = middle(0.77f, 0.83f), head = middle(0.86f, 0.96f);
    // The shoulders: most of the way out to what is widest about the chest at their height.
    const auto band = middle(0.74f, 0.80f);
    float reach = 0;
    for (const auto& p : positions)
        if (p[1] >= lo[1] + 0.74f * h && p[1] <= lo[1] + 0.80f * h) reach = std::max(reach, std::fabs(p[0] - chest[0]));
    const float side = std::max(0.6f * reach, 0.08f * h);
    // The legs: each side's middle below the hips (left on +x), at hip, knee and ankle height.
    auto legMiddle = [&](float from, float to, float sign) {
        double x = 0, z = 0;
        size_t count = 0;
        for (const auto& p : positions)
            if (p[1] >= lo[1] + from * h && p[1] <= lo[1] + to * h && (p[0] - hips[0]) * sign > 0) x += p[0], z += p[2], ++count;
        return count ? std::array<float, 2>{float(x / double(count)), float(z / double(count))}
                     : std::array<float, 2>{hips[0] + sign * 0.08f * h, hips[1]};
    };
    // The elbows: halfway out along a T-pose's arms (reaching past a quarter of the height), else
    // halfway down arms hanging by the sides.
    const bool tpose = reach > 0.25f * h;
    const float elbowOut = tpose ? side + 0.5f * (reach - side) : side, elbowDown = tpose ? 0 : 0.17f * h;
    Rig rig;
    rig.bones = {{"BASE", -1, {cx, lo[1], cz}},
                 {"BACKA", 0, at(hips, 0.5f)},
                 {"BACKB", 1, at(chest, 0.68f)},
                 {"LF_H", 1, at(legMiddle(0.36f, 0.44f, 1), 0.47f)},
                 {"RT_H", 1, at(legMiddle(0.36f, 0.44f, -1), 0.47f)},
                 {"NECK1", 2, at(neck, 0.8f)},
                 {"LF_S", 2, {chest[0] + side, lo[1] + 0.78f * h, band[1]}},
                 {"RT_S", 2, {chest[0] - side, lo[1] + 0.78f * h, band[1]}},
                 {"LF_K", 3, at(legMiddle(0.22f, 0.30f, 1), 0.26f)},
                 {"RT_K", 4, at(legMiddle(0.22f, 0.30f, -1), 0.26f)},
                 {"HEAD", 5, at(head, 0.86f)},
                 {"LF_E", 6, {chest[0] + elbowOut, lo[1] + 0.78f * h - elbowDown, band[1]}},
                 {"RT_E", 7, {chest[0] - elbowOut, lo[1] + 0.78f * h - elbowDown, band[1]}},
                 {"LF_F", 8, at(legMiddle(0.02f, 0.10f, 1), 0.05f)},
                 {"RT_F", 9, at(legMiddle(0.02f, 0.10f, -1), 0.05f)}};
    return rig;
}

void skinWeights(const Rig& rig, const std::array<float, 3>& pos, std::array<uint8_t, 4>& bones, std::array<uint8_t, 4>& weights) {
    if (rig.bones.empty()) fail("skinWeights: the rig has no bones");
    // The spine: the bones not on a side (LF_/RT_), by height. Each owns from midway up to it from the
    // one below to midway up to the next, and neighbours blend smoothly across those midpoints.
    std::vector<int> spine;
    for (size_t i = 0; i < rig.bones.size(); ++i)
        if (!startsWith(rig.bones[i].name, "LF_") && !startsWith(rig.bones[i].name, "RT_")) spine.push_back(int(i));
    if (spine.empty()) spine.push_back(0);
    std::stable_sort(spine.begin(), spine.end(), [&](int a, int b) { return rig.bones[size_t(a)].world[1] < rig.bones[size_t(b)].world[1]; });
    auto height = [&](size_t k) { return rig.bones[size_t(spine[k])].world[1]; };
    const size_t last = spine.size() - 1;
    std::vector<float> centre(spine.size());
    for (size_t k = 0; k < last; ++k) centre[k] = (height(k) + height(k + 1)) / 2;
    centre[last] = last ? height(last) + (height(last) - height(last - 1)) / 2 : height(last);
    std::map<int, float> w;
    const float y = pos[1];
    if (y <= centre[0]) {
        w[spine[0]] = 1;
    } else if (y >= centre[last]) {
        w[spine[last]] = 1;
    } else {
        size_t k = 0;
        while (k + 1 < last && y >= centre[k + 1]) ++k;
        const float u = smooth(centre[k], centre[k + 1], y);
        w[spine[k]] += 1 - u;
        w[spine[k + 1]] += u;
    }
    // How far along the joint a-b a place lies (0 at a, 1 at b).
    auto along = [&](int a, int b) {
        const auto& p = rig.bones[size_t(a)].world;
        const auto& q = rig.bones[size_t(b)].world;
        const float d[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
        const float len = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        return len > 0 ? ((pos[0] - p[0]) * d[0] + (pos[1] - p[1]) * d[1] + (pos[2] - p[2]) * d[2]) / len : 0.0f;
    };
    // Height in units of the spine's (its top joint at 0.86 of the mesh's height).
    const float tall = last && height(last) > height(0) ? (height(last) - height(0)) / 0.86f : 1.0f;
    // The legs: below its hip joint, each side's leg, from hip to knee to ankle.
    const int pelvis = boneIndex(rig, "BACKA");
    for (const auto& [hipName, kneeName, footName, sign] :
         {std::tuple{"LF_H", "LF_K", "LF_F", 1.0f}, std::tuple{"RT_H", "RT_K", "RT_F", -1.0f}}) {
        const int hip = boneIndex(rig, hipName), knee = boneIndex(rig, kneeName), foot = boneIndex(rig, footName);
        if (hip < 0 || knee < 0 || foot < 0 || pelvis < 0 || (pos[0] - rig.bones[size_t(pelvis)].world[0]) * sign < 0) continue;
        const float hy = rig.bones[size_t(hip)].world[1];
        const float leg = 1 - smooth(hy - 0.05f * tall, hy + 0.02f * tall, y);
        if (leg <= 0) continue;
        const float kneeShare = smooth(0.55f, 1.0f, along(hip, knee)), footShare = smooth(0.55f, 1.0f, along(knee, foot));
        for (auto& [b, x] : w) x *= 1 - leg;
        w[hip] += leg * (1 - kneeShare);
        w[knee] += leg * kneeShare * (1 - footShare);
        w[foot] += leg * kneeShare * footShare;
    }
    // The arms: out past a shoulder (from its distance from the chest to 1.4 times it), between the
    // hips and the neck; past halfway to its elbow, more and more the elbow's.
    for (const char* name : {"LF_S", "RT_S"}) {
        const int s = boneIndex(rig, name);
        if (s < 0) continue;
        const Bone& sh = rig.bones[size_t(s)];
        const float from = sh.parent >= 0 ? rig.bones[size_t(sh.parent)].world[0] : 0;
        const float off = sh.world[0] - from;
        if (std::fabs(off) < 1e-6f) continue;
        const float out = (pos[0] - from) * (off > 0 ? 1.0f : -1.0f), reach = std::fabs(off);
        const float arm = smooth(reach, 1.4f * reach, out) * smooth(sh.world[1] - 0.36f * tall, sh.world[1] - 0.26f * tall, y) *
                          (1 - smooth(sh.world[1] + 0.04f * tall, sh.world[1] + 0.08f * tall, y));
        if (arm <= 0) continue;
        for (auto& [b, x] : w) x *= 1 - arm;
        const int elbow = boneIndex(rig, std::string(name).substr(0, 3) + "E");
        const float elbowShare = elbow >= 0 ? smooth(0.5f, 1.0f, along(s, elbow)) : 0.0f;
        w[s] += arm * (1 - elbowShare);
        if (elbow >= 0) w[elbow] += arm * elbowShare;
    }
    // The four heaviest, in 255ths that sum to 255 (the heaviest takes the rounding).
    std::vector<std::pair<int, float>> list(w.begin(), w.end());
    std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    list.resize(std::min<size_t>(list.size(), 4));
    float total = 0;
    for (auto& [b, x] : list) total += x;
    bones = {};
    weights = {};
    int sum = 0;
    for (size_t k = 0; k < list.size(); ++k) {
        bones[k] = uint8_t(list[k].first);
        weights[k] = uint8_t(std::lround(255.0f * list[k].second / total));
        sum += weights[k];
    }
    weights[0] = uint8_t(weights[0] + 255 - sum);
}

Bytes writeSkinnedModel(const std::string& assetName, const Rig& rig, const std::vector<SkinDraw>& draws, const std::vector<std::string>& textures,
                        const blockmodel::ShapeTable* shapes) {
    blockmodel::Skin skin;
    skin.bones = skinBones(rig);
    std::vector<blockmodel::WorldDraw> world;
    for (const SkinDraw& d : draws) {
        blockmodel::WorldDraw w;
        w.indices = d.indices;
        w.texture = d.texture;
        std::vector<std::array<uint32_t, 2>> words;
        for (const SkinVertex& v : d.vertices) {
            w.vertices.push_back(v.v);
            if (shapes) w.shapes.push_back(v.shapes);
            // Each word big-endian with component 0 in its low byte: what the stream's 8-in-32 swap and
            // an 8_8_8_8 fetch give back as x, y, z, w. Weights are made to sum to 255.
            int sum = 0;
            for (uint8_t x : v.weights) sum += x;
            if (!sum) fail(assetName + ": a vertex has no bone weights");
            std::array<uint8_t, 4> wt{};
            int given = 0;
            for (size_t k = 0; k < 4; ++k) given += wt[k] = uint8_t(v.weights[k] * 255 / sum);
            wt[size_t(std::max_element(v.weights.begin(), v.weights.end()) - v.weights.begin())] += uint8_t(255 - given);
            uint32_t idx = 0, wgt = 0;
            for (size_t k = 0; k < 4; ++k) {
                if (wt[k] && v.bones[k] >= skin.bones.size())
                    fail(assetName + ": a vertex is weighted to bone " + std::to_string(v.bones[k]) + ", which the rig doesn't have");
                idx |= uint32_t(wt[k] ? v.bones[k] : 0) << (8 * k);
                wgt |= uint32_t(wt[k]) << (8 * k);
            }
            words.push_back({idx, wgt});
        }
        world.push_back(std::move(w));
        skin.words.push_back(std::move(words));
    }
    return blockmodel::writeSkinned(assetName, world, textures, skin, shapes);
}

Bytes writeIdleAnim(const std::string& assetName, const Rig& rig, const std::string& idle, float seconds, float armsDown) {
    const std::vector<blockmodel::SkinBone> bones = skinBones(rig);
    const uint32_t n = uint32_t(bones.size());
    if (!(seconds > 0)) seconds = kDefaultSeconds;
    const long counted = std::lround(double(seconds) * kFps) + 1;
    if (counted > 0xFFFF) fail(assetName + ": an animation of at most 65534 frames (" + std::to_string(int(0xFFFE / kFps)) + " s)");
    const uint32_t frames = uint32_t(std::max(2L, counted));
    const float duration = float(frames - 1) / kFps;

    // The keys: local rotations (bind rotations are identity), offsets from the bind positions, scales.
    std::vector<model::Key> keys;
    if (idle == "breathe") {
        keys = breathe(rig, frames, duration);
    } else if (idle == "walk") {
        keys = walk(rig, frames, duration);
    } else {
        model::Animation a;
        a.name = assetName;
        a.duration = duration;
        a.frames = a.authoredFrames = int(frames);
        a.bones = n;
        for (uint32_t f = 0; f < frames; ++f) a.frameTimes.push_back(float(f) / kFps);
        for (const blockmodel::SkinBone& b : bones) {
            model::Bone mb;
            mb.name = b.name;
            mb.parent = b.parent;
            const std::array<float, 3> up = b.parent < 0 ? std::array<float, 3>{0, 0, 0} : bones[size_t(b.parent)].world;
            for (size_t k = 0; k < 3; ++k) mb.local[k] = b.world[k] - up[k], mb.world[k] = b.world[k];
            a.pose.push_back(mb);
            a.boneNames.push_back(b.name);
            a.parents.push_back(b.parent);
        }
        a.keys.assign(size_t(frames) * n, model::Key{});
        keys = model::keysFromJson(a, readText(idle));
    }
    // Arms lowered first (about z: the left, on +x, turns down), then each frame's own turn on top.
    if (armsDown != 0) {
        for (const auto& [name, sign] : {std::pair<const char*, double>{"LF_S", -1.0}, {"RT_S", 1.0}}) {
            const int bone = boneIndex(rig, name);
            if (bone < 0) continue;
            const std::array<float, 4> down = euler(0, 0, sign * armsDown);
            for (uint32_t f = 0; f < frames; ++f) {
                float* q = keys[size_t(f) * n + size_t(bone)].rotation;
                const float a[4] = {q[0], q[1], q[2], q[3]};
                const float* b = down.data();
                q[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
                q[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
                q[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
                q[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
            }
        }
    }
    return writeAnim(assetName, rig, keys, frames);
}

Bytes writeAnim(const std::string& assetName, const Rig& rig, const std::vector<model::Key>& keys, uint32_t frames) {
    const std::vector<blockmodel::SkinBone> bones = skinBones(rig);
    const std::vector<int> depth = blockmodel::detail::boneDepths(bones);
    const uint32_t n = uint32_t(bones.size());
    if (frames < 2 || frames > 0xFFFF || keys.size() != size_t(frames) * n) fail(assetName + ": wrong number of keys");
    const float duration = float(frames - 1) / kFps;
    // Translations in steps fitting the farthest (2^-15 m reaches 1 m); rotations and scales in the game's own steps.
    float far = 0;
    for (const model::Key& k : keys)
        for (float x : k.translation) far = std::max(far, std::fabs(x));
    const float move = std::max(fromBits(0x38000000), far / 32000.0f);
    const std::vector<float> scale = {fromBits(0x38000100), fromBits(0x38000100), fromBits(0x38000100), move, move, move,
                                      fromBits(0x38000000), fromBits(0x38000000), fromBits(0x38000000)};
    const std::vector<float> def = {0, 0, 0, 0, 0, 0, 1, 1, 1};
    const model::EncodedTrack track = model::encodeTrack(assetName, keys, frames, n, scale, def);

    // The bone names in the string pool, as the game's animations keep them (each with its length and
    // NUL, the ELF hash of both, and how many pointers name it).
    Bytes pool;
    std::vector<caff::PoolString> strings;
    std::vector<uint32_t> names;
    for (const blockmodel::SkinBone& b : bones) {
        names.push_back(uint32_t(pool.size()));
        const std::string z = b.name + std::string(1, '\0');
        strings.push_back({uint32_t(z.size()), uint32_t(pool.size()), blockmodel::detail::elfHash(z), 2});
        pool.insert(pool.end(), z.begin(), z.end());
    }

    // The header ("animation" 19.12.06.0036), laid out as aid_anim_banjox_objects_showdowntown_tamperswitch's.
    Sym d;
    d.str("animation");
    d.str("19.12.06.0036");
    d.u32(0);
    d.f32(duration);
    d.u16(uint16_t(n));
    d.u16(uint16_t(frames));
    const uint32_t hTrack = d.ptr(kData), hTrackB = d.ptr(kData), hTrackC = d.ptr(kData);
    d.u32(0);                                    // +0x30 no root motion
    const uint32_t hMask = d.ptr(kData), hNames = d.ptr(kData);
    d.u32(0), d.u32(0), d.u32(0);
    const uint32_t hPose = d.ptr(kData), hDepths = d.ptr(kData);
    d.u32(0x000A0000);
    d.u32(0), d.u32(0);
    d.f32(1.0f);                                 // +0x5C every authored frame stored
    d.u32(0);

    // The rotation track: QUAT_BITSTREAM (the narrow form) and its parameters {1.0, ->scales, ->defaults,
    // ->masks, ->data, ->widths, base and width bytes, frame bytes, 9 channels, 12 floats, records, frames}.
    d.set(hTrack, d.pos());
    d.str("QUAT_BITSTREAM");
    d.zeros(24 - 15);
    d.ptr(kData, d.pos() + 4);
    d.f32(1.0f);
    const uint32_t pScale = d.ptr(kData), pDefault = d.ptr(kData), pMasks = d.ptr(kData), pData = d.ptr(kData), pWidths = d.ptr(kData);
    d.u16(uint16_t(track.bases.size()));
    d.u16(uint16_t(track.widths.size()));
    d.u16(uint16_t(track.stride));
    d.u16(9);
    d.u16(12);
    d.u16(uint16_t(n));
    d.u16(uint16_t(frames));
    d.b.push_back(0), d.b.push_back(0);
    d.set(pScale, d.pos());
    for (float x : scale) d.f32(x);
    d.set(pDefault, d.pos());
    for (float x : def) d.f32(x);
    d.set(pMasks, d.pos());
    d.b.insert(d.b.end(), track.masks.begin(), track.masks.end());
    d.align(16);
    d.set(pData, d.pos());
    d.b.insert(d.b.end(), track.bases.begin(), track.bases.end());
    d.set(pWidths, d.pos());
    d.b.insert(d.b.end(), track.widths.begin(), track.widths.end());
    d.b.insert(d.b.end(), track.frames.begin(), track.frames.end());

    // The two other tracks, empty.
    for (uint32_t at : {hTrackB, hTrackC}) {
        d.align(4);
        d.set(at, d.pos());
        d.str("BITSTREAM");
        d.zeros(24 - 10);
        d.u32(0);
    }

    // How many bones are at each depth or above it, from 0 to all of them.
    d.set(hDepths, d.pos());
    const int deepest = depth.back();
    d.u16(0);
    for (int k = 0; k <= deepest; ++k) d.u16(uint16_t(std::count_if(depth.begin(), depth.end(), [&](int x) { return x <= k; })));
    d.align(4);

    // The skeleton (the model's pose, its names in the pool), every bone animated, the names again.
    d.set(hPose, blockmodel::detail::pose(d, bones, kPool, names));
    d.align(4);
    d.set(hMask, d.pos());
    for (uint32_t word = 0; word < (n + 31) / 32; ++word) {
        const uint32_t left = n - 32 * word;
        d.u32(left >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> left));
    }
    d.set(hNames, blockmodel::detail::nameTable(d, bones, kPool, names));
    d.align(16);

    // The CAFF: .data holding the animation and its pool (the section's first symbol names the pool).
    caff::Caff c;
    c.symbolNames = {assetName, "pool"};
    caff::Section s;
    s.name = ".data";
    s.alignLog2 = 4;
    c.sections.push_back(s);
    c.appendSymbol(1, 1, 4, d.b);
    c.appendSymbol(2, 1, 5, pool);
    while (c.sections[0].data.size() % 16) c.sections[0].data.push_back(0);
    c.sections[0].firstSymbol = 2;
    std::map<uint32_t, std::vector<uint32_t>> byTarget;
    for (auto& [at, to] : d.relocs) byTarget[to].push_back(at);
    for (auto& [to, offsets] : byTarget) c.relocs[0].push_back({kData, to, offsets});
    c.pools.push_back({kPool, strings});
    return c.serialize();
}

Bytes writeAnimtable(const std::string& assetName, const std::vector<AnimEntry>& entries) {
    if (entries.empty()) fail(assetName + ": an animation table needs an entry");
    std::vector<AnimEntry> sorted = entries;
    std::stable_sort(sorted.begin(), sorted.end(), [](const AnimEntry& a, const AnimEntry& b) { return a.name < b.name; });
    // Entries {char name[64], anim id, animevents id (none), 0, 0, blend in and out 0.2 s, flags}; the
    // game's loops ("stand", "walk", "run") are 0x00402000, its one-shots 0x01402000.
    Bytes data(8 + 0x5C * sorted.size(), 0);
    putBe32(&data[0], uint32_t(sorted.size()));
    for (size_t i = 0; i < sorted.size(); ++i) {
        const AnimEntry& e = sorted[i];
        if (e.name.empty() || e.name.size() > 63) fail(assetName + ": an animation's name is 1 to 63 characters (" + e.name + ")");
        if (i && e.name == sorted[i - 1].name) fail(assetName + ": two animations are named " + e.name);
        const auto id = assetdb::idFromName(e.anim);
        if (!id || (*id >> 24) != 0x02) fail(assetName + ": " + e.anim + " is not an anim asset's name");
        uint8_t* p = &data[8 + 0x5C * i];
        std::memcpy(p, e.name.data(), e.name.size());
        putBe32(p + 0x40, *id);
        putBe32(p + 0x50, 0x3E4CCCCD);
        putBe32(p + 0x54, 0x3E4CCCCD);
        putBe32(p + 0x58, (e.loop ? 0x00402000u : 0x01402000u) | e.layer);
    }
    return oneAsset(assetName, 4, data);
}

} // namespace nut::npcmodel
