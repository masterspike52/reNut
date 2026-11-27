// COLLADA (.dae) scenes as a SceneMesh (gltfobj::daeScene): every geometry its visual scene places,
// with its material's texture or colour and its vertex colours.
#include "gltfobj.hpp"

#include "image.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>

namespace nut::gltfobj {
namespace {

// ---- A small XML reader: elements, attributes and text (entities decoded), no namespaces --------

struct Element {
    std::string name, text;
    std::map<std::string, std::string> attrs;
    std::vector<std::unique_ptr<Element>> children;

    const Element* child(const std::string& n) const {
        for (auto& c : children)
            if (c->name == n) return c.get();
        return nullptr;
    }
    std::vector<const Element*> all(const std::string& n) const {
        std::vector<const Element*> out;
        for (auto& c : children)
            if (c->name == n) out.push_back(c.get());
        return out;
    }
    std::string attr(const std::string& n) const {
        auto it = attrs.find(n);
        return it == attrs.end() ? std::string() : it->second;
    }
};

class XmlReader {
public:
    explicit XmlReader(const std::string& s) : s_(s) {
        if (s_.size() >= 3 && std::memcmp(s_.data(), "\xEF\xBB\xBF", 3) == 0) p_ = 3;
    }

    std::unique_ptr<Element> document() {
        skipMisc();
        if (p_ >= s_.size() || s_[p_] != '<') fail("dae: no root element");
        return element();
    }

private:
    [[noreturn]] void bad(const std::string& what) const { fail("dae: " + what + " at byte " + std::to_string(p_)); }
    bool starts(const char* t) const { return s_.compare(p_, std::strlen(t), t) == 0; }
    void skipTo(const char* t) {
        const size_t at = s_.find(t, p_);
        if (at == std::string::npos) bad(std::string("no closing ") + t);
        p_ = at + std::strlen(t);
    }
    void skipSpace() {
        while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_;
    }
    // Declarations, comments, doctypes and processing instructions between elements.
    void skipMisc() {
        for (;;) {
            skipSpace();
            if (starts("<?")) skipTo("?>");
            else if (starts("<!--")) skipTo("-->");
            else if (starts("<!")) skipTo(">");
            else return;
        }
    }
    static void decode(std::string& t) {
        if (t.find('&') == std::string::npos) return;
        static const std::pair<const char*, char> kEntities[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
        std::string out;
        for (size_t i = 0; i < t.size();) {
            bool done = false;
            for (auto& [e, ch] : kEntities)
                if (t.compare(i, std::strlen(e), e) == 0) {
                    out += ch;
                    i += std::strlen(e);
                    done = true;
                    break;
                }
            if (!done) out += t[i++];
        }
        t = std::move(out);
    }
    std::string name() {
        const size_t at = p_;
        while (p_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[p_])) && s_[p_] != '>' && s_[p_] != '/' && s_[p_] != '=') ++p_;
        if (p_ == at) bad("a name was expected");
        return s_.substr(at, p_ - at);
    }
    std::unique_ptr<Element> element() {
        ++p_;  // '<'
        auto e = std::make_unique<Element>();
        e->name = name();
        for (;;) {
            skipSpace();
            if (p_ >= s_.size()) bad("an unclosed tag");
            if (starts("/>")) {
                p_ += 2;
                return e;
            }
            if (s_[p_] == '>') {
                ++p_;
                break;
            }
            const std::string key = name();
            skipSpace();
            if (p_ >= s_.size() || s_[p_] != '=') bad("an attribute without a value");
            ++p_;
            skipSpace();
            const char quote = p_ < s_.size() ? s_[p_] : 0;
            if (quote != '"' && quote != '\'') bad("an unquoted attribute");
            const size_t end = s_.find(quote, p_ + 1);
            if (end == std::string::npos) bad("an unclosed attribute");
            std::string value = s_.substr(p_ + 1, end - p_ - 1);
            decode(value);
            e->attrs[key] = std::move(value);
            p_ = end + 1;
        }
        for (;;) {
            const size_t lt = s_.find('<', p_);
            if (lt == std::string::npos) bad("<" + e->name + "> is not closed");
            e->text.append(s_, p_, lt - p_);
            p_ = lt;
            if (starts("</")) {
                skipTo(">");
                decode(e->text);
                return e;
            }
            if (starts("<![CDATA[")) {
                const size_t end = s_.find("]]>", p_);
                if (end == std::string::npos) bad("an unclosed CDATA section");
                e->text.append(s_, p_ + 9, end - p_ - 9);
                p_ = end + 3;
            } else if (starts("<!--") || starts("<?")) {
                skipMisc();
            } else {
                e->children.push_back(element());
            }
        }
    }

    const std::string& s_;
    size_t p_ = 0;
};

// ---- COLLADA -------------------------------------------------------------------------------------

using V3 = std::array<float, 3>;
using Mat = std::array<float, 16>;  // row-major, as COLLADA writes it

Mat identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat mul(const Mat& a, const Mat& b) {
    Mat r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[size_t(4 * i + j)] += a[size_t(4 * i + k)] * b[size_t(4 * k + j)];
    return r;
}

V3 apply(const Mat& m, const V3& p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

std::vector<float> floats(const std::string& text) {
    std::vector<float> out;
    const char* p = text.c_str();
    char* end = nullptr;
    for (;;) {
        const float f = std::strtof(p, &end);
        if (end == p) break;
        out.push_back(f);
        p = end;
    }
    return out;
}

std::vector<long> ints(const std::string& text) {
    std::vector<long> out;
    const char* p = text.c_str();
    char* end = nullptr;
    for (;;) {
        const long v = std::strtol(p, &end, 10);
        if (end == p) break;
        out.push_back(v);
        p = end;
    }
    return out;
}

std::string unref(const std::string& url) { return !url.empty() && url[0] == '#' ? url.substr(1) : url; }

// A node's own transform: its <matrix>, <translate>, <rotate> and <scale> elements in order.
Mat transformOf(const Element& node) {
    Mat m = identity();
    for (auto& c : node.children) {
        const std::vector<float> v = floats(c->text);
        Mat t = identity();
        if (c->name == "matrix" && v.size() >= 16) {
            std::copy(v.begin(), v.begin() + 16, t.begin());
        } else if (c->name == "translate" && v.size() >= 3) {
            t[3] = v[0], t[7] = v[1], t[11] = v[2];
        } else if (c->name == "scale" && v.size() >= 3) {
            t[0] = v[0], t[5] = v[1], t[10] = v[2];
        } else if (c->name == "rotate" && v.size() >= 4) {
            const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len <= 0) continue;
            const float x = v[0] / len, y = v[1] / len, z = v[2] / len, a = v[3] * 3.14159265f / 180;
            const float c0 = std::cos(a), s = std::sin(a), k = 1 - c0;
            t = {x * x * k + c0, x * y * k - z * s, x * z * k + y * s, 0, y * x * k + z * s, y * y * k + c0, y * z * k - x * s, 0,
                 z * x * k - y * s, z * y * k + x * s, z * z * k + c0, 0, 0, 0, 0, 1};
        } else {
            continue;
        }
        m = mul(m, t);
    }
    return m;
}

// A <source>: its float array and accessor stride.
struct Source {
    std::vector<float> values;
    size_t stride = 1;
    size_t count() const { return stride ? values.size() / stride : 0; }
};

struct Material {
    std::array<float, 4> colour{1, 1, 1, 1};
    int image = -1;
    blockmodel::WorldMaterial look;  // its emission, transparency and double-sidedness, for reNut
    bool hasLook = false;            // an effect gave it (else no material record)
};

} // namespace

SceneMesh daeScene(const fs::path& dae, float smoothDegrees, const std::vector<std::string>& waterNames) {
    const fs::path dir = dae.parent_path();
    const std::string text = readText(dae);
    const std::unique_ptr<Element> root = XmlReader(text).document();
    if (root->name != "COLLADA") fail("dae: " + dae.string() + " is not a COLLADA file");
    auto library = [&](const char* n) { return root->child(n); };

    // Z-up files turn to Y-up as the game is.
    bool zUp = false;
    if (const Element* asset = root->child("asset"))
        if (const Element* up = asset->child("up_axis")) zUp = trim(up->text) == "Z_UP";

    SceneMesh out;
    // Images, read as their materials need them.
    std::map<std::string, std::string> imageFile;  // image id -> file
    if (const Element* lib = library("library_images"))
        for (const Element* img : lib->all("image"))
            if (const Element* from = img->child("init_from")) {
                // COLLADA 1.5 nests the file in <ref>.
                std::string f = trim(from->child("ref") ? from->child("ref")->text : from->text);
                if (f.rfind("file:///", 0) == 0) f = f.substr(8);
                else if (f.rfind("file://", 0) == 0) f = f.substr(7);
                std::string decoded;
                for (size_t i = 0; i < f.size(); ++i) {
                    if (f[i] == '%' && i + 2 < f.size() && std::isxdigit(static_cast<unsigned char>(f[i + 1])) &&
                        std::isxdigit(static_cast<unsigned char>(f[i + 2]))) {
                        decoded += char(std::strtol(f.substr(i + 1, 2).c_str(), nullptr, 16));
                        i += 2;
                    } else {
                        decoded += f[i];
                    }
                }
                imageFile[img->attr("id")] = decoded;
            }
    std::map<std::string, int> loaded;  // image id -> index in out.images
    auto loadImage = [&](const std::string& id) {
        auto [it, fresh] = loaded.emplace(id, -1);
        if (!fresh) return it->second;
        auto f = imageFile.find(id);
        if (f == imageFile.end()) {
            out.warnings.push_back("dae: no image " + id + "; its materials are untextured");
            return -1;
        }
        const fs::path path = fs::path(f->second).is_absolute() ? fs::path(f->second) : dir / f->second;
        try {
            image::Rgba img = image::decodeImage(readFile(path));
            if (!img.width || !img.height || img.pixels.size() < size_t(img.width) * img.height * 4) fail("it has no pixels");
            it->second = int(out.images.size());
            out.images.push_back(std::move(img));
            out.imageNames.push_back(path.string());
        } catch (const std::exception& e) {
            out.warnings.push_back(path.string() + " was not read (" + e.what() + "); its materials are untextured");
        }
        return it->second;
    };

    // Effects: a diffuse texture (through its sampler and surface params, or naming an image) or colour.
    std::map<std::string, Material> effects;
    if (const Element* lib = library("library_effects"))
        for (const Element* fx : lib->all("effect")) {
            Material m;
            const Element* profile = fx->child("profile_COMMON");
            if (!profile) {
                effects[fx->attr("id")] = m;
                continue;
            }
            std::map<std::string, std::string> samplerSurface, surfaceImage;
            for (const Element* p : profile->all("newparam")) {
                if (const Element* s2 = p->child("sampler2D")) {
                    if (const Element* src = s2->child("source")) samplerSurface[p->attr("sid")] = trim(src->text);
                    else if (const Element* inst = s2->child("instance_image")) surfaceImage[p->attr("sid")] = unref(inst->attr("url"));
                }
                if (const Element* surf = p->child("surface"))
                    if (const Element* from = surf->child("init_from")) surfaceImage[p->attr("sid")] = trim(from->text);
            }
            const Element* technique = profile->child("technique");
            const Element* shading = nullptr;
            if (technique)
                for (const char* n : {"phong", "blinn", "lambert", "constant"})
                    if (!shading) shading = technique->child(n);
            const Element* diffuse = shading ? shading->child("diffuse") : nullptr;
            if (diffuse) {
                if (const Element* tex = diffuse->child("texture")) {
                    std::string id = tex->attr("texture");
                    if (samplerSurface.count(id)) id = samplerSurface[id];
                    if (surfaceImage.count(id)) id = surfaceImage[id];
                    m.image = loadImage(id);
                } else if (const Element* col = diffuse->child("color")) {
                    const std::vector<float> v = floats(col->text);
                    for (size_t k = 0; k < 4 && k < v.size(); ++k) m.colour[k] = std::clamp(v[k], 0.0f, 1.0f);
                }
            }
            m.hasLook = true;
            // Emission: a colour, or a texture giving off its own.
            if (const Element* emission = shading ? shading->child("emission") : nullptr) {
                if (const Element* tex = emission->child("texture")) {
                    std::string id = tex->attr("texture");
                    if (samplerSurface.count(id)) id = samplerSurface[id];
                    if (surfaceImage.count(id)) id = surfaceImage[id];
                    m.look.emissiveTexture = loadImage(id);
                    if (m.look.emissiveTexture >= 0) m.look.emissive = {1, 1, 1};
                } else if (const Element* col = emission->child("color")) {
                    const std::vector<float> v = floats(col->text);
                    for (size_t k = 0; k < 3 && k < v.size(); ++k) m.look.emissive[k] = std::clamp(v[k], 0.0f, 1.0f);
                }
            }
            // Transparency below 1 blends (RGB_ZERO files give it the other way round).
            if (const Element* tr = shading ? shading->child("transparency") : nullptr)
                if (const Element* f = tr->child("float")) {
                    float opacity = std::clamp(float(std::atof(trim(f->text).c_str())), 0.0f, 1.0f);
                    const Element* transparent = shading->child("transparent");
                    if (transparent && transparent->attr("opaque") == "RGB_ZERO") opacity = 1 - opacity;
                    if (opacity < 0.99f) {
                        m.look.opacity = opacity;
                        m.look.alpha = blockmodel::WorldMaterial::Blend;
                    }
                }
            // Exporters' double-sided flag, in an <extra> of the effect or its profile.
            for (const Element* holder : {static_cast<const Element*>(fx), profile, technique})
                if (holder)
                    for (const Element* extra : holder->all("extra"))
                        for (const Element* tq : extra->all("technique"))
                            if (const Element* ds = tq->child("double_sided")) m.look.doubleSided = std::atoi(trim(ds->text).c_str()) != 0;
            effects[fx->attr("id")] = m;
        }
    std::map<std::string, Material> materials;
    if (const Element* lib = library("library_materials"))
        for (const Element* mat : lib->all("material"))
            if (const Element* inst = mat->child("instance_effect")) materials[mat->attr("id")] = effects[unref(inst->attr("url"))];

    std::map<std::string, const Element*> geometries;
    if (const Element* lib = library("library_geometries"))
        for (const Element* g : lib->all("geometry")) geometries[g->attr("id")] = g;

    // Welded places, triangles over them with their corners and images, as objScene gathers them.
    std::vector<V3> pos;
    std::map<std::array<long, 3>, int> weld;
    std::vector<std::array<int, 3>> tris;
    std::vector<std::array<Corner, 3>> triCorners;
    std::vector<int> triImage, triWater, triMaterial;
    std::vector<SceneMesh::Water> water;
    std::vector<SceneMesh::Material> sceneMaterials;
    int badColours = 0;

    // `waterName`: the water object it is (its triangles go to `water`), or "".
    auto addGeometry = [&](const Element& geom, const Mat& world, const std::map<std::string, std::string>& bound,
                           const std::string& waterName) {
        const Element* mesh = geom.child("mesh");
        if (!mesh) return;
        std::map<std::string, Source> sources;
        for (const Element* s : mesh->all("source")) {
            Source src;
            if (const Element* fa = s->child("float_array")) src.values = floats(fa->text);
            if (const Element* tc = s->child("technique_common"))
                if (const Element* acc = tc->child("accessor")) src.stride = std::max(1L, std::atol(acc->attr("stride").c_str()));
            sources[s->attr("id")] = std::move(src);
        }
        // <vertices> gathers per-vertex inputs (its POSITION and any others) under one id.
        std::map<std::string, std::vector<std::pair<std::string, std::string>>> vertexInputs;
        if (const Element* v = mesh->child("vertices"))
            for (const Element* in : v->all("input")) vertexInputs[v->attr("id")].push_back({in->attr("semantic"), unref(in->attr("source"))});

        for (auto& prim : mesh->children) {
            const bool triangles = prim->name == "triangles", polylist = prim->name == "polylist", polygons = prim->name == "polygons";
            if (!triangles && !polylist && !polygons) continue;
            // Each input's source and offset; the lowest TEXCOORD set is the one used.
            const Source *position = nullptr, *texcoord = nullptr, *colour = nullptr;
            size_t posAt = 0, texAt = 0, colAt = 0, stride = 1;
            long texSet = 1L << 30;
            for (const Element* in : prim->all("input")) {
                const size_t offset = size_t(std::atol(in->attr("offset").c_str()));
                stride = std::max(stride, offset + 1);
                const std::string semantic = in->attr("semantic"), source = unref(in->attr("source"));
                auto take = [&](const std::string& sem, const std::string& id) {
                    auto it = sources.find(id);
                    if (it == sources.end()) return;
                    if (sem == "POSITION") position = &it->second, posAt = offset;
                    else if (sem == "COLOR" && !colour) colour = &it->second, colAt = offset;
                    else if (sem == "TEXCOORD") {
                        const long set = in->attrs.count("set") ? std::atol(in->attr("set").c_str()) : 0;
                        if (set < texSet) texSet = set, texcoord = &it->second, texAt = offset;
                    }
                };
                if (semantic == "VERTEX")
                    for (auto& [sem, id] : vertexInputs[source]) take(sem, id);
                else
                    take(semantic, source);
            }
            if (!position) continue;
            // Its material, through the instance's bindings or by its own id.
            const std::string symbol = prim->attr("material");
            auto b = bound.find(symbol);
            const std::string materialId = b != bound.end() ? b->second : symbol;
            auto m = materials.find(materialId);
            const Material material = m != materials.end() ? m->second : Material{};
            const int materialIndex = m != materials.end() && material.hasLook ? addMaterial(sceneMaterials, materialId, material.look) : -1;
            // Water by its object, else its material's name or its texture's.
            const std::string texture = material.image >= 0 ? out.imageNames[size_t(material.image)] : std::string();
            const std::string primWater = !waterName.empty()                       ? waterName
                                          : isWaterName(materialId, waterNames) ? materialId
                                          : isWaterName(texture, waterNames)    ? texture
                                                                                : std::string();

            // The polygons, each a list of index tuples.
            std::vector<long> indices;
            std::vector<size_t> sizes;
            for (const Element* p : prim->all("p")) {
                const std::vector<long> v = ints(p->text);
                indices.insert(indices.end(), v.begin(), v.end());
                if (polygons) sizes.push_back(v.size() / stride);
            }
            if (polylist)
                if (const Element* vc = prim->child("vcount"))
                    for (long n : ints(vc->text)) sizes.push_back(size_t(std::max(0L, n)));
            if (triangles) sizes.assign(indices.size() / stride / 3, 3);

            // Vertex colours are used only when every one the block names is a colour.
            bool coloursGood = colour != nullptr;
            if (colour)
                for (size_t i = colAt; i < indices.size() && coloursGood; i += stride) {
                    const long ci = indices[i];
                    if (ci < 0 || size_t(ci) >= colour->count()) coloursGood = false;
                    else
                        for (size_t k = 0; k < std::min<size_t>(colour->stride, 4); ++k) {
                            const float c = colour->values[size_t(ci) * colour->stride + k];
                            if (!(c >= -0.01f && c <= 1.01f)) coloursGood = false;
                        }
                }
            if (colour && !coloursGood) ++badColours;

            size_t at = 0;
            for (size_t n : sizes) {
                if ((at + n) * stride > indices.size()) break;
                std::vector<int> place(n);
                std::vector<Corner> corner(n);
                bool texOk = material.image >= 0 && texcoord != nullptr;
                for (size_t k = 0; k < n; ++k) {
                    const long* tuple = &indices[(at + k) * stride];
                    const long pi = tuple[posAt];
                    if (pi < 0 || size_t(pi) >= position->count()) fail("dae: a polygon names a position its source doesn't have");
                    const float* pv = &position->values[size_t(pi) * position->stride];
                    V3 p = apply(world, {pv[0], position->stride > 1 ? pv[1] : 0, position->stride > 2 ? pv[2] : 0});
                    if (zUp) p = {p[0], p[2], -p[1]};
                    const std::array<long, 3> key{std::lround(p[0] * 1e4f), std::lround(p[1] * 1e4f), std::lround(p[2] * 1e4f)};
                    auto [it, fresh] = weld.emplace(key, int(pos.size()));
                    if (fresh) pos.push_back(p);
                    place[k] = it->second;

                    Corner& c = corner[k];
                    for (size_t q = 0; q < 4; ++q) c.colour[q] = material.image >= 0 ? (q < 3 ? 1.0f : material.colour[3]) : material.colour[q];
                    if (coloursGood) {
                        const float* cv = &colour->values[size_t(tuple[colAt]) * colour->stride];
                        for (size_t q = 0; q < std::min<size_t>(colour->stride, 4); ++q) c.colour[q] *= std::clamp(cv[q], 0.0f, 1.0f);
                    }
                    if (texOk) {
                        const long ti = tuple[texAt];
                        if (ti < 0 || size_t(ti) >= texcoord->count() || texcoord->stride < 2) texOk = false;
                        else {
                            const float* tv = &texcoord->values[size_t(ti) * texcoord->stride];
                            c.uv = {tv[0], 1 - tv[1]};  // COLLADA's t runs up from the image's bottom
                        }
                    }
                }
                // A polygon as a fan, its degenerate triangles dropped.
                for (size_t k = 1; k + 1 < n; ++k) {
                    const std::array<int, 3> tri{place[0], place[k], place[k + 1]};
                    if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;
                    triWater.push_back(primWater.empty()
                                           ? -1
                                           : addWater(water, primWater, {pos[size_t(tri[0])], pos[size_t(tri[1])], pos[size_t(tri[2])]}));
                    std::array<Corner, 3> cs{corner[0], corner[k], corner[k + 1]};
                    if (!texOk)
                        for (Corner& c : cs) c.uv = {0, 0};
                    tris.push_back(tri);
                    triCorners.push_back(cs);
                    triImage.push_back(texOk ? material.image : -1);
                    triMaterial.push_back(materialIndex);
                }
                at += n;
            }
        }
    };

    // The visual scene's nodes, with their parents' transforms; without one, every geometry as it is.
    std::function<void(const Element&, const Mat&)> walk = [&](const Element& node, const Mat& parent) {
        const Mat world = mul(parent, transformOf(node));
        const std::string nodeName = !node.attr("name").empty() ? node.attr("name") : node.attr("id");
        for (const Element* ig : node.all("instance_geometry")) {
            std::map<std::string, std::string> bound;
            if (const Element* bm = ig->child("bind_material"))
                if (const Element* tc = bm->child("technique_common"))
                    for (const Element* im : tc->all("instance_material")) bound[im->attr("symbol")] = unref(im->attr("target"));
            auto g = geometries.find(unref(ig->attr("url")));
            if (g == geometries.end()) continue;
            const std::string geomName = !g->second->attr("name").empty() ? g->second->attr("name") : g->first;
            addGeometry(*g->second, world, bound,
                        isWaterName(nodeName, waterNames) ? nodeName : isWaterName(geomName, waterNames) ? geomName : "");
        }
        for (const Element* c : node.all("node")) walk(*c, world);
    };
    const Element* scenes = library("library_visual_scenes");
    const Element* visual = scenes ? scenes->child("visual_scene") : nullptr;
    if (visual)
        for (const Element* node : visual->all("node")) walk(*node, identity());
    else
        for (auto& [id, g] : geometries) {
            const std::string geomName = !g->attr("name").empty() ? g->attr("name") : id;
            addGeometry(*g, identity(), {}, isWaterName(geomName, waterNames) ? geomName : "");
        }

    if (tris.empty()) fail("dae: " + dae.string() + " has no triangles");
    if (badColours)
        out.warnings.push_back("dae: " + std::to_string(badColours) + " mesh(es) have vertex colours out of range; they are drawn without them");
    SceneMesh result = sceneFromTriangles(pos, tris, triCorners, triImage, smoothDegrees);
    result.images = std::move(out.images);
    result.imageNames = std::move(out.imageNames);
    result.warnings = std::move(out.warnings);
    result.water = std::move(water);
    result.triangleWater = std::move(triWater);
    result.materials = std::move(sceneMaterials);
    result.triangleMaterial = std::move(triMaterial);
    return result;
}

} // namespace nut::gltfobj
