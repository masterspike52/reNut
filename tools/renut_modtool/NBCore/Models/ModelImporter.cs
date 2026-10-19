using System.Numerics;
using NB.Core.Formats;
using NB.Core.IO;

namespace NB.Core.Models;

/// <summary>
/// Replaces the geometry of an aid_model with imported meshes, keeping its materials, shaders and vertex format.
///
/// Every reference to GPU memory in a model is one of (see docs/FORMATS.md §6):
///   VB record (.data):  +0 stride, +4 → runtime object, +8 → .gpu vertices, +0xC byte size
///   GPU buffer table:   R+0x48 → entries (→ VB record+4, → .gpu, size), count R+0x50
///   IB table:           R+0x54 → entries (→ IB object, → .gpu, size, format 1 = 16-bit), count R+0x58
///   draw commands (.stream ops 0x01/0x30): primitive, index count, → IB object
/// New vertex/index data is appended to the .gpu part; those records are repointed (the relocation entries stay
/// valid because pointers are part-relative offsets). Old data is left in place (unreferenced).
///
/// Mesh i replaces the i-th distinct vertex buffer (in draw order) and all of its draws (LODs) use the new index
/// list. Vertex buffers without a mesh keep their data but draw a single degenerate triangle (i.e. nothing).
/// Attributes the mesh does not provide (vertex colour, extra channels, tangent sign) are copied from the nearest
/// original vertex of that buffer. Chunk 5 (bounds) is recomputed.
/// </summary>
public static class ModelImporter
{
    public sealed record Result(int VertexBuffers, int Vertices, int Triangles, List<string> Notes);

    /// <summary>Largest chunk of one material placed into one vertex buffer (several materials can share a buffer,
    /// whose total must stay below 65,536 vertices).</summary>
    public const int MaxChunkVertices = 60000;   // 16-bit indices allow 65,535

    public static Result Replace(CaffFile caff, int symbol, IReadOnlyList<ImportMesh> meshes, bool spatialTiles = false, ISet<int>? keepVbRecords = null, ISet<int>? excludeVbRecords = null,
        bool forceByMaterial = false, bool uniformVertexColour = false, bool trimHidden = false, bool avoidLayered = false)
    {
        var m = ModelAsset.Parse(caff, symbol);
        // spatial tiling (terrain): new geometry bounds, original culling layout (root box + centre of every draw block)
        (Vector3, Vector3, Vector3, Vector3, Dictionary<int, Vector3>)? tileSpace = null;
        if (spatialTiles)
        {
            var (rootOff, cells) = ModelEdit.GetCullCells(caff, symbol);
            var allP = meshes.SelectMany(x => x.Positions).ToList();
            var live = cells.Where(c => c.Group >= 0).ToList();
            if (allP.Count > 0 && live.Count > 0)
                tileSpace = (allP.Aggregate(Vector3.Min), allP.Aggregate(Vector3.Max),
                             live.Select(c => c.Min).Aggregate(Vector3.Min), live.Select(c => c.Max).Aggregate(Vector3.Max),
                             live.GroupBy(c => c.Group).ToDictionary(g => g.Key, g => (g.First().Min + g.First().Max) / 2));
        }
        if (m.ResourceHeader < 0 || m.Draws.Count == 0) throw new InvalidDataException("model has no resolvable draws: " + string.Join("; ", m.Warnings));
        var v = m.View;
        var d = v.Data(".data"); var s = v.Data(".stream");
        var gpuPart = v.Part(v.PartId(".gpu"));
        var gpu = new List<byte>(gpuPart.Data);
        int R = m.ResourceHeader;
        var notes = new List<string>();

        int vbTable = v.PtrAt(".data", R + 0x48)?.Offset ?? throw new InvalidDataException("no GPU buffer table");
        int vbCount = BE.S32(d, R + 0x50);
        int ibTable = v.PtrAt(".data", R + 0x54)!.Value.Offset; int ibCount = BE.S32(d, R + 0x58);

        int Append(byte[] bytes)
        {
            while (gpu.Count % 32 != 0) gpu.Add(0);
            int off = gpu.Count; gpu.AddRange(bytes); return off;
        }
        void SetVb(int record, int gpuOff, int size)
        {
            BE.W32(d, record + 8, gpuOff); BE.W32(d, record + 12, size);
            bool found = false;
            for (int i = 0; i < vbCount; i++)
            {
                int e = vbTable + 12 * i;
                if (v.PtrAt(".data", e)?.Offset == record + 4) { BE.W32(d, e + 4, gpuOff); BE.W32(d, e + 8, size); found = true; }
            }
            if (!found) throw new InvalidDataException($"VB record 0x{record:X} not in the GPU buffer table");
        }
        void SetIb(int ibObject, int gpuOff, int size)
        {
            for (int i = 0; i < ibCount; i++)
            {
                int e = ibTable + 16 * i;
                if (BE.S32(d, e) == ibObject || v.PtrAt(".data", e)?.Offset == ibObject) { BE.W32(d, e + 4, gpuOff); BE.W32(d, e + 8, size); return; }
            }
            throw new InvalidDataException($"IB object 0x{ibObject:X} not in the index buffer table");
        }
        // .data also keeps (→ IB object, index count) pairs (a per-draw table the engine draws from; found at R+0x00 /
        // R+0x20 in props). They must match the new counts, or the old count is drawn from the new buffer.
        var ibCountLocs = new Dictionary<int, List<int>>();
        foreach (var ((part, off), to) in v.Pointers)
            if (part == v.PartId(".data") && to == v.PartId(".data") && off + 8 <= d.Length)
            {
                int target = BE.S32(d, off);
                uint cnt = BE.U32(d, off + 4);
                if (m.Draws.Any(x => x.IbObject == target) && cnt > 0 && cnt < 0x1000000 && !(off >= ibTable && off < ibTable + 16 * ibCount))
                    (ibCountLocs.TryGetValue(target, out var l) ? l : ibCountLocs[target] = new()).Add(off + 4);
            }
        void SetDrawCounts(int ibObject, int count)
        {
            foreach (var loc in ibCountLocs.GetValueOrDefault(ibObject) ?? new()) BE.W32(d, loc, count);
            int pos = v.PtrAt(".stream", 0)?.Offset ?? 0x24; int n = 0;
            while (pos + 4 <= s.Length)
            {
                uint w = BE.U32(s, pos); int size = (int)(w >> 16), op = (int)((w >> 8) & 0xFF);
                if ((w & 0xFF) != 0 || size < 4 || pos + size > s.Length) break;
                if ((op == 0x01 || op == 0x30) && v.PtrAt(".stream", pos + 12)?.Offset == ibObject)
                {
                    BE.W32(s, pos + 4, 4); BE.W32(s, pos + 8, count);   // triangle list; op 0x30 +16 = LOD level (kept)
                    n++;
                }
                if (op == 0x1D) break;
                pos += size;
            }
            if (n == 0) throw new InvalidDataException($"no draw command uses IB 0x{ibObject:X}");
        }

        // instanced draws (index = vertex*4 + instance) would draw imported geometry with random instance transforms:
        // with a material mapping their buffers are simply left out (hidden unless kept); otherwise refuse
        var instancedVbs = m.Draws.Where(x => x.Instanced).Select(x => x.VbRecord).ToHashSet();
        if (instancedVbs.Count > 0 && meshes.Count > 0)   // no meshes at all: every draw (instanced ones too) draws nothing
        {
            bool mapped = meshes.Any(x => m.Draws.Any(d => ObjExporter.DiffuseTexture(d) is string t && string.Equals(ObjExporter.TextureFileStem(t), x.Name, StringComparison.OrdinalIgnoreCase)));
            if (!mapped) throw new InvalidDataException("this model uses instanced draws (index = vertex*4 + instance); imported geometry would be drawn with random instance transforms. Use a model without instanced draws as the target/template (see NB.Cli model-survey: 'instanced 0'), or import by material.");
            excludeVbRecords = new HashSet<int>((excludeVbRecords ?? new HashSet<int>()).Concat(instancedVbs));
            notes.Add($"{instancedVbs.Count} instanced vertex buffer(s) left out of the import");
        }
        var groups = m.Draws.GroupBy(x => x.VbRecord).ToList();
        int missing = m.Draws.Select(x => x.IbObject).Distinct().Count(ib => !ibCountLocs.ContainsKey(ib));
        if (missing > 0) notes.Add($"warning: {missing} index buffer(s) have no (IB, count) entry in .data — their counts were only updated in the draw commands");
        // Mapping: one mesh per draw when the counts match (e.g. an OBJ exported by this tool: LODs preserved),
        // otherwise one mesh per vertex buffer, used by all of that buffer's draws (LODs).
        // By material: when mesh names match the draws' diffuse texture names (the material names ObjExporter writes and
        // Blender keeps), every draw gets the merged meshes of its own material — draws that share a vertex buffer
        // (brass / wood / slate of one building) keep separate materials. LOD draws of one material share the geometry.
        // Draws whose material has no mesh draw nothing.
        string? MatOf(MeshDraw x) => ObjExporter.DiffuseTexture(x) is string t ? ObjExporter.TextureFileStem(t) : null;
        var byName = meshes.GroupBy(x => x.Name, StringComparer.OrdinalIgnoreCase).ToDictionary(g => g.Key, g => Merge(g.ToList()), StringComparer.OrdinalIgnoreCase);
        bool byMaterial = (forceByMaterial || meshes.Count != m.Draws.Count) && m.Draws.Any(x => MatOf(x) is string mt && byName.ContainsKey(mt));
        bool perDraw = !byMaterial && meshes.Count == m.Draws.Count;
        if (byMaterial)
        {
            var used = m.Draws.Select(MatOf).Where(x => x != null && byName.ContainsKey(x)).Distinct().ToList();
            notes.Add($"mapping: by material — {used.Count} of {byName.Count} imported material(s) matched: {string.Join(", ", used)}");
            var unmatched = byName.Keys.Where(k => !used.Contains(k, StringComparer.OrdinalIgnoreCase)).ToList();
            if (unmatched.Count > 0) notes.Add($"warning: imported material(s) with no draw of that texture in the model (not imported): {string.Join(", ", unmatched)}");
        }
        else notes.Add(perDraw ? $"mapping: one mesh per draw ({m.Draws.Count})" : $"mapping: one mesh per vertex buffer ({groups.Count}); all LODs of a buffer use its mesh");
        // By material, each material's mesh is split into chunks of at most MaxChunkVertices vertices; chunk i goes to
        // the i-th vertex buffer holding that material (all of that buffer's draws of the material, i.e. its LODs, get
        // it). The material's other buffers draw nothing. Terrain models have dozens of spatial chunks per material;
        // giving each the whole mesh would draw it dozens of times.
        var chunkFor = new Dictionary<(string, int), ImportMesh>();
        if (byMaterial)
            foreach (var (mat, mesh) in byName)
            {
                // buffer suitability: UVs (required when any buffer has them), float32 positions (half floats lose
                // precision on world coordinates), no vertex-colour blend channel, fewest texture layers
                bool HasUv(int vb) => m.Draws.First(x => x.VbRecord == vb).Layout.Any(e => e.Format is VtxFormat.k_16_16_FLOAT or VtxFormat.k_32_32_FLOAT);
                bool Pos32(int vb) { var l = m.Draws.First(x => x.VbRecord == vb).Layout; return (l.FirstOrDefault(e => e.Offset == 0) ?? l.First()).Format is VtxFormat.k_32_32_32_FLOAT or VtxFormat.k_32_32_32_32_FLOAT; }
                bool VCol(int vb) => m.Draws.First(x => x.VbRecord == vb).Layout.Any(e => e.Format == VtxFormat.k_8_8_8_8);
                int Layers(int vb) => m.Draws.Where(x => x.VbRecord == vb).Max(x => x.Textures.Count);
                // only buffers drawn at LOD 0 (created models keep LOD 0 forever; buffers of LOD 1+ draws are never shown)
                var all = m.Draws.Where(x => string.Equals(MatOf(x), mat, StringComparison.OrdinalIgnoreCase) && !m.LodOnlyNodes.Contains(x.Node) && keepVbRecords?.Contains(x.VbRecord) != true && excludeVbRecords?.Contains(x.VbRecord) != true).Select(x => x.VbRecord).Distinct().ToList();
                float span = mesh.Positions.Count == 0 ? 0 : mesh.Positions.Max(p => MathF.Max(MathF.Abs(p.X), MathF.Max(MathF.Abs(p.Y), MathF.Abs(p.Z))));
                // avoidLayered (terrain): a buffer whose draws carry a blend / transparency mask or a second colour
                // texture is a layer drawn over another one (masked, see-through); a whole tile imported there showed as
                // see-through holes and dark patches (ULTRA Lab hills, convoy round 3). Such buffers are used last.
                bool Layered(int vb) => m.Draws.Where(x => x.VbRecord == vb).Any(x => x.Textures.Skip(1).Any(t =>
                    t.Texture.Contains("blend") || t.Texture.Contains("transparency") || t.Texture.Contains("_mask") ||
                    (t.Texture.Contains("_colour") && !t.Texture.Contains("specular")) || t.Texture.StartsWith("aid_texture_banjox_seattle_") || t.Texture.StartsWith("aid_texture_banjox_lab_")));
                var vbs = all.Where(vb => (!all.Any(HasUv) || HasUv(vb)) && (span < 256 || Pos32(vb)))
                    .OrderBy(vb => VCol(vb) ? 1 : 0).ThenBy(Layers).ToList();
                if (avoidLayered)
                {
                    var clean = vbs.Where(vb => !Layered(vb) && !VCol(vb)).ToList();
                    var noLayer = vbs.Where(vb => !Layered(vb)).ToList();
                    int need = (mesh.Positions.Count + MaxChunkVertices - 1) / MaxChunkVertices;
                    var pick = clean.Count >= need ? clean : noLayer.Count >= need ? noLayer : vbs;
                    if (pick.Count < vbs.Count) notes.Add($"material {mat}: {pick.Count} of {vbs.Count} buffers used (layered/vertex-colour buffers left out)");
                    vbs = pick;
                }
                if (vbs.Count == 0) throw new InvalidDataException($"material {mat}: no vertex buffer with texture coordinates{(span >= 256 ? " and float32 positions" : "")} to hold the imported mesh");
                if (vbs.Count == 0) continue;
                var parts = spatialTiles && tileSpace != null ? SpatialTiles(mesh, MaxChunkVertices, vbs.Count, TileTriangles) : Split(mesh, MaxChunkVertices);
                if (parts.Count > vbs.Count) throw new InvalidDataException($"material {mat}: the mesh needs {parts.Count} vertex buffers of at most {MaxChunkVertices} vertices, but the model has only {vbs.Count} buffer(s) with this material");
                if (spatialTiles && tileSpace != null)
                {
                    // each tile goes to the free buffer whose draw block sat at the same relative place in the original
                    // layout, so a block collects neighbouring tiles of every material and its culling box stays small
                    var (nmn, nmx, omn, omx, blockCentre) = tileSpace.Value;
                    Vector2 Norm(Vector3 p, Vector3 a, Vector3 b) => new((p.X - a.X) / MathF.Max(b.X - a.X, 1e-3f), (p.Z - a.Z) / MathF.Max(b.Z - a.Z, 1e-3f));
                    Vector2 VbPos(int vb) { var blk = m.Draws.First(x => x.VbRecord == vb).Block; return blockCentre.TryGetValue(blk, out var c) ? Norm(c, omn, omx) : new(0.5f, 0.5f); }
                    var free = vbs.ToList();
                    foreach (var tile in parts.OrderByDescending(p => p.Triangles.Count))
                    {
                        var tc = tile.Positions.Aggregate(Vector3.Zero, (acc, p) => acc + p) / Math.Max(1, tile.Positions.Count);
                        var tn = Norm(tc, nmn, nmx);
                        int best = free.OrderBy(vb => Vector2.DistanceSquared(VbPos(vb), tn)).First();
                        free.Remove(best);
                        chunkFor[(mat.ToLowerInvariant(), best)] = tile;
                    }
                }
                else for (int i = 0; i < parts.Count; i++) chunkFor[(mat.ToLowerInvariant(), vbs[i])] = parts[i];
                if (parts.Count > 1 || vbs.Count > 1) notes.Add($"material {mat}: {mesh.Positions.Count} vertices in {parts.Count} chunk(s) over {vbs.Count} buffer(s) holding it");
            }
        int totalV = 0, totalT = 0;
        var allPos = new List<Vector3>();
        for (int gi = 0; gi < groups.Count; gi++)
        {
            var draws = groups[gi].ToList();
            var proto = draws[0];
            if (keepVbRecords?.Contains(proto.VbRecord) == true) { notes.Add($"vertex buffer {gi}: kept (original geometry)"); continue; }
            var drawMesh = draws.Select(x => byMaterial ? (MatOf(x) is string mt && chunkFor.TryGetValue((mt.ToLowerInvariant(), x.VbRecord), out var bm) ? bm : null)
                : perDraw ? meshes[m.Draws.IndexOf(x)] : gi < meshes.Count ? meshes[gi] : null).ToList();
            if (drawMesh.All(x => x == null))
            {
                int off = Append(new byte[8]);   // indices 0,0,0 (+pad)
                foreach (var ibo in draws.Select(x => x.IbObject).Distinct()) { SetIb(ibo, off, 6); SetDrawCounts(ibo, 3); }
                // trimHidden: the buffer shrinks to one zero vertex (the degenerate triangle reads only vertex 0), so a
                // clone used for one material does not carry the template's other geometry (GPU compaction drops it)
                if (trimHidden) SetVb(proto.VbRecord, Append(new byte[Math.Max(32, proto.Stride)]), Math.Max(4, proto.Stride));
                notes.Add($"vertex buffer {gi}: hidden (no mesh for it){(trimHidden ? ", trimmed" : "")}");
                continue;
            }
            // one vertex buffer holding every distinct mesh of this group, each draw indexing its own slice
            var distinct = drawMesh.Where(x => x != null).Distinct().ToList();
            var combined = new ImportMesh { Name = string.Join("+", distinct.Select(x => x!.Name).Distinct()), Normals = new(), UVs = distinct.All(x => x!.UVs != null) ? new() : null,
                UVs2 = distinct.All(x => x!.UVs2 != null) ? new() : null };
            var baseOf = new Dictionary<ImportMesh, int>();
            foreach (var x in distinct)
            {
                baseOf[x!] = combined.Positions.Count;
                combined.Positions.AddRange(x!.Positions); combined.Normals.AddRange(x.Normals!);
                if (combined.UVs != null) combined.UVs.AddRange(x.UVs!);
                if (combined.UVs2 != null) combined.UVs2.AddRange(x.UVs2!);
                combined.Triangles.AddRange(x.Triangles.Select(t => t + baseOf[x]));
            }
            if (combined.Positions.Count > 0xFFFF) throw new InvalidDataException($"vertex buffer {gi} would need {combined.Positions.Count} vertices; at most 65535 fit 16-bit indices");
            var vb = EncodeVertices(proto, gpuPart.Data, BE.S32(d, proto.VbRecord + 8), combined, notes, uniformVertexColour);
            int vbOff = Append(vb);
            SetVb(proto.VbRecord, vbOff, vb.Length);
            var ibDone = new HashSet<int>();
            for (int k = 0; k < draws.Count; k++)
            {
                var mesh = drawMesh[k];
                if (!ibDone.Add(draws[k].IbObject)) continue;
                var tris = mesh == null ? new List<int> { 0, 0, 0 } : mesh.Triangles.Select(t => t + baseOf[mesh]).ToList();
                var ib = new byte[(tris.Count * 2 + 3) & ~3];
                for (int i = 0; i < tris.Count; i++) BE.W16(ib, 2 * i, (ushort)tris[i]);
                int ibOff = Append(ib);
                SetIb(draws[k].IbObject, ibOff, tris.Count * 2); SetDrawCounts(draws[k].IbObject, tris.Count);
                totalT += tris.Count / 3;
            }
            totalV += combined.Positions.Count;
            allPos.AddRange(combined.Positions);
            notes.Add($"vertex buffer {gi}: '{combined.Name}' {combined.Positions.Count} vertices, {draws.Count} draw(s)");
        }
        if (!perDraw && !byMaterial && meshes.Count > groups.Count) notes.Add($"{meshes.Count - groups.Count} extra mesh(es) ignored: the model has {groups.Count} vertex buffers and {m.Draws.Count} draws");
        while (gpu.Count % 32 != 0) gpu.Add(0);
        gpuPart.Data = gpu.ToArray();
        if (allPos.Count > 0 && m.Chunks.TryGetValue(5, out int b5)) WriteBounds(d, b5, allPos);
        return new Result(groups.Count, totalV, totalT, notes);
    }

    /// <summary>Triangles per spatial tile aimed for by <see cref="SpatialTiles"/> (terrain imports).</summary>
    public static int TileTriangles = 1500;

    /// <summary>
    /// Splits a mesh into spatial tiles (k-d split on triangle centroids along x or z, largest tile first) until every
    /// tile has at most <paramref name="targetTris"/> triangles or <paramref name="maxTiles"/> is reached; tiles also stay
    /// under <paramref name="maxVerts"/> vertices. Used for terrain so the engine's per-block culling can skip what is
    /// not in view (B22).
    /// </summary>
    public static List<ImportMesh> SpatialTiles(ImportMesh m, int maxVerts, int maxTiles, int targetTris)
    {
        var tiles = new List<List<int>> { Enumerable.Range(0, m.Triangles.Count / 3).ToList() };
        Vector3 Cen(int t) => (m.Positions[m.Triangles[3 * t]] + m.Positions[m.Triangles[3 * t + 1]] + m.Positions[m.Triangles[3 * t + 2]]) / 3;
        while (tiles.Count < maxTiles)
        {
            var big = tiles.OrderByDescending(x => x.Count).First();
            if (big.Count <= targetTris && big.Count * 3 <= maxVerts || big.Count < 2) break;
            var cs = big.Select(t => (t, c: Cen(t))).ToList();
            float sx = cs.Max(x => x.c.X) - cs.Min(x => x.c.X), sz = cs.Max(x => x.c.Z) - cs.Min(x => x.c.Z);
            var sorted = sx >= sz ? cs.OrderBy(x => x.c.X).ToList() : cs.OrderBy(x => x.c.Z).ToList();
            int half = sorted.Count / 2;
            tiles.Remove(big);
            tiles.Add(sorted.Take(half).Select(x => x.t).ToList());
            tiles.Add(sorted.Skip(half).Select(x => x.t).ToList());
        }
        var res = new List<ImportMesh>();
        foreach (var tl in tiles)
        {
            var sub = new ImportMesh { Name = m.Name, Normals = m.Normals != null ? new() : null, UVs = m.UVs != null ? new() : null, UVs2 = m.UVs2 != null ? new() : null };
            foreach (var t in tl) for (int k = 0; k < 3; k++) sub.Triangles.Add(m.Triangles[3 * t + k]);
            // re-index the tile's vertices
            var map = new Dictionary<int, int>(); var tri = new List<int>();
            foreach (var v in sub.Triangles)
            {
                if (!map.TryGetValue(v, out int nv)) { nv = sub.Positions.Count; map[v] = nv; sub.Positions.Add(m.Positions[v]); sub.Normals?.Add(m.Normals![v]); sub.UVs?.Add(m.UVs![v]); sub.UVs2?.Add(m.UVs2![v]); }
                tri.Add(nv);
            }
            sub.Triangles = tri;
            res.AddRange(sub.Positions.Count > maxVerts ? Split(sub, maxVerts) : new() { sub });
        }
        return res;
    }

    /// <summary>Splits a mesh into pieces of at most <paramref name="maxVerts"/> vertices (whole triangles, vertices
    /// re-indexed per piece).</summary>
    public static List<ImportMesh> Split(ImportMesh m, int maxVerts)
    {
        if (m.Positions.Count <= maxVerts) return new() { m };
        var parts = new List<ImportMesh>();
        ImportMesh? cur = null; Dictionary<int, int>? map = null;
        for (int t = 0; t + 2 < m.Triangles.Count; t += 3)
        {
            if (cur == null || map!.Count + 3 > maxVerts)
            {
                cur = new ImportMesh { Name = m.Name, Normals = m.Normals != null ? new() : null, UVs = m.UVs != null ? new() : null, UVs2 = m.UVs2 != null ? new() : null };
                map = new(); parts.Add(cur);
            }
            for (int k = 0; k < 3; k++)
            {
                int v = m.Triangles[t + k];
                if (!map.TryGetValue(v, out int nv))
                {
                    nv = cur.Positions.Count; map[v] = nv;
                    cur.Positions.Add(m.Positions[v]);
                    cur.Normals?.Add(m.Normals![v]);
                    cur.UVs?.Add(m.UVs![v]);
                    cur.UVs2?.Add(m.UVs2![v]);
                }
                cur.Triangles.Add(nv);
            }
        }
        return parts;
    }

    /// <summary>Concatenates meshes (same material) into one.</summary>
    static ImportMesh Merge(List<ImportMesh> list)
    {
        if (list.Count == 1) return list[0];
        var r = new ImportMesh { Name = list[0].Name, Normals = new(), UVs = list.All(x => x.UVs != null) ? new() : null, UVs2 = list.All(x => x.UVs2 != null) ? new() : null };
        foreach (var x in list)
        {
            int b = r.Positions.Count;
            r.Positions.AddRange(x.Positions); r.Normals.AddRange(x.Normals ?? ObjReader.ComputeNormals(x));
            if (r.UVs != null) r.UVs.AddRange(x.UVs!);
            if (r.UVs2 != null) r.UVs2.AddRange(x.UVs2!);
            r.Triangles.AddRange(x.Triangles.Select(t => t + b));
        }
        return r;
    }

    /// <summary>Chunk 5: r(origin), AABB centre, r(centre), base point, height, max |x|,|z|, AABB min, AABB max.</summary>
    static void WriteBounds(byte[] d, int o, List<Vector3> p)
    {
        var mn = new Vector3(float.MaxValue); var mx = new Vector3(float.MinValue);
        foreach (var x in p) { mn = Vector3.Min(mn, x); mx = Vector3.Max(mx, x); }
        var c = (mn + mx) / 2;
        float rO = p.Max(x => x.Length()), rC = p.Max(x => Vector3.Distance(x, c));
        float[] f = { rO, c.X, c.Y, c.Z, rC, c.X, mn.Y, c.Z, mx.Y - mn.Y, MathF.Max(MathF.Max(MathF.Abs(mn.X), MathF.Abs(mx.X)), MathF.Max(MathF.Abs(mn.Z), MathF.Abs(mx.Z))), mn.X, mn.Y, mn.Z, mx.X, mx.Y, mx.Z };
        for (int i = 0; i < f.Length; i++) BE.WF32(d, o + 4 * i, f[i]);
    }

    static byte[] EncodeVertices(MeshDraw proto, byte[] gpu, int oldOff, ImportMesh mesh, List<string> notes, bool uniformColour = false)
    {
        int stride = proto.Stride, n = mesh.Positions.Count;
        var lay = proto.Layout;
        var pos = lay.FirstOrDefault(e => e.Offset == 0) ?? lay.First();
        var uv = lay.FirstOrDefault(e => e.Format is VtxFormat.k_16_16_FLOAT or VtxFormat.k_32_32_FLOAT);
        // Every 2-component float channel gets the mesh UVs: some shaders sample the colour texture through a second
        // UV set (Showdown Town supportstrut: two 16_16_FLOAT channels); copying it from the nearest original vertex made
        // the whole mesh one texel.
        var uvs = lay.Where(e => e.Format is VtxFormat.k_16_16_FLOAT or VtxFormat.k_32_32_FLOAT).ToList();
        var vecs = lay.Where(e => e != pos && e.Format is VtxFormat.k_2_10_10_10 or VtxFormat.k_10_11_11 or VtxFormat.k_11_11_10).ToList();
        var nrm = vecs.FirstOrDefault(); var tan = vecs.Skip(1).FirstOrDefault();
        var tangents = tan != null ? Tangents(mesh) : null;
        var old = proto.Positions;
        var grid = new NearestGrid(old);
        var outp = new byte[n * stride];
        // uniform vertex colour: the average of the original buffer's 8_8_8_8 channels (baked shading/tint of the old
        // shape), so a new model does not inherit blotches from its nearest old vertices
        var colEls = uniformColour ? lay.Where(e => e.Format == VtxFormat.k_8_8_8_8).ToList() : new();
        var colAvg = new List<byte[]>();
        foreach (var ce in colEls)
        {
            var sum = new long[4]; int cnt = Math.Max(1, old.Length);
            for (int k = 0; k < old.Length; k++) for (int c = 0; c < 4; c++) sum[c] += gpu[oldOff + k * stride + ce.Offset + c];
            colAvg.Add(sum.Select(x => (byte)(old.Length == 0 ? 255 : x / cnt)).ToArray());
        }
        for (int i = 0; i < n; i++)
        {
            int dst = i * stride;
            int src = old.Length > 0 ? grid.Nearest(mesh.Positions[i]) : -1;
            if (src >= 0) Array.Copy(gpu, oldOff + src * stride, outp, dst, stride);
            var p = mesh.Positions[i];
            Store(outp, dst + pos.Offset, pos, new Vector4(p, 1));
            if (nrm != null && mesh.Normals != null) Store(outp, dst + nrm.Offset, nrm, new Vector4(mesh.Normals[i], ModelAsset.Fetch(outp, dst + nrm.Offset, nrm).W));
            if (tan != null && tangents != null) Store(outp, dst + tan.Offset, tan, new Vector4(tangents[i], ModelAsset.Fetch(outp, dst + tan.Offset, tan).W));
            for (int u = 0; u < uvs.Count; u++)
            {
                // a second UV set (lightmap / AO coordinates) goes to the second float2 channel when the mesh has one
                var t = u == 1 && mesh.UVs2 != null ? mesh.UVs2[i] : mesh.UVs != null ? mesh.UVs[i] : Vector2.Zero;
                Store(outp, dst + uvs[u].Offset, uvs[u], new Vector4(t, 0, 1));
            }
            for (int c = 0; c < colEls.Count; c++) Array.Copy(colAvg[c], 0, outp, dst + colEls[c].Offset, 4);
        }
        if (uv != null && mesh.UVs == null) notes.Add($"'{mesh.Name}': no texture coordinates in the OBJ (UVs set to 0)");
        return outp;
    }

    static Vector3[] Tangents(ImportMesh m)
    {
        var t = new Vector3[m.Positions.Count];
        if (m.UVs != null)
            for (int i = 0; i + 2 < m.Triangles.Count; i += 3)
            {
                int a = m.Triangles[i], b = m.Triangles[i + 1], c = m.Triangles[i + 2];
                var e1 = m.Positions[b] - m.Positions[a]; var e2 = m.Positions[c] - m.Positions[a];
                var d1 = m.UVs[b] - m.UVs[a]; var d2 = m.UVs[c] - m.UVs[a];
                float r = d1.X * d2.Y - d2.X * d1.Y;
                if (MathF.Abs(r) < 1e-12f) continue;
                var tt = (e1 * d2.Y - e2 * d1.Y) / r;
                t[a] += tt; t[b] += tt; t[c] += tt;
            }
        for (int i = 0; i < t.Length; i++)
        {
            var nn = m.Normals![i];
            var x = t[i] - nn * Vector3.Dot(nn, t[i]);   // Gram-Schmidt
            if (x.LengthSquared() < 1e-12f) x = Vector3.Cross(nn, MathF.Abs(nn.Y) < 0.9f ? Vector3.UnitY : Vector3.UnitX);
            t[i] = Vector3.Normalize(x);
        }
        return t;
    }

    /// <summary>Writes one attribute — the exact inverse of <see cref="ModelAsset.Fetch"/>.</summary>
    public static void Store(byte[] g, int o, VertexElement e, Vector4 v)
    {
        switch (e.Format)
        {
            case VtxFormat.k_32_32_32_FLOAT: BE.WF32(g, o, v.X); BE.WF32(g, o + 4, v.Y); BE.WF32(g, o + 8, v.Z); break;
            case VtxFormat.k_32_32_32_32_FLOAT: BE.WF32(g, o, v.X); BE.WF32(g, o + 4, v.Y); BE.WF32(g, o + 8, v.Z); BE.WF32(g, o + 12, v.W); break;
            case VtxFormat.k_32_32_FLOAT: BE.WF32(g, o, v.X); BE.WF32(g, o + 4, v.Y); break;
            case VtxFormat.k_32_FLOAT: BE.WF32(g, o, v.X); break;
            case VtxFormat.k_16_16_FLOAT: BE.W16(g, o, BitConverter.HalfToUInt16Bits((Half)v.X)); BE.W16(g, o + 2, BitConverter.HalfToUInt16Bits((Half)v.Y)); break;
            case VtxFormat.k_16_16_16_16_FLOAT:
                BE.W16(g, o, BitConverter.HalfToUInt16Bits((Half)v.X)); BE.W16(g, o + 2, BitConverter.HalfToUInt16Bits((Half)v.Y));
                BE.W16(g, o + 4, BitConverter.HalfToUInt16Bits((Half)v.Z)); BE.W16(g, o + 6, BitConverter.HalfToUInt16Bits((Half)v.W)); break;
            case VtxFormat.k_16_16:
            case VtxFormat.k_16_16_16_16:
            {
                int nc = e.Format == VtxFormat.k_16_16 ? 2 : 4;
                float[] c = { v.X, v.Y, v.Z, v.W };
                for (int k = 0; k < nc; k++)
                {
                    float x = e.Normalized ? c[k] * (e.Signed ? 32767f : 65535f) : c[k];
                    int q = (int)MathF.Round(x);
                    q = e.Signed ? Math.Clamp(q, -32768, 32767) : Math.Clamp(q, 0, 65535);
                    BE.W16(g, o + 2 * k, (ushort)q);
                }
                break;
            }
            case VtxFormat.k_2_10_10_10:
            {
                uint Q(float x, int bits)
                {
                    int max = (1 << (bits - (e.Signed ? 1 : 0))) - 1;
                    int q = (int)MathF.Round(e.Normalized ? x * max : x);
                    q = e.Signed ? Math.Clamp(q, -(1 << (bits - 1)), (1 << (bits - 1)) - 1) : Math.Clamp(q, 0, (1 << bits) - 1);
                    return (uint)q & ((1u << bits) - 1);
                }
                BE.W32(g, o, Q(v.X, 10) | Q(v.Y, 10) << 10 | Q(v.Z, 10) << 20 | Q(v.W, 2) << 30);
                break;
            }
            case VtxFormat.k_10_11_11:
            case VtxFormat.k_11_11_10:
            {
                int[] bits = e.Format == VtxFormat.k_10_11_11 ? new[] { 11, 11, 10 } : new[] { 10, 11, 11 };
                float[] c = { v.X, v.Y, v.Z }; uint w = 0; int sh = 0;
                for (int k = 0; k < 3; k++)
                {
                    int b = bits[k]; int max = (1 << (b - (e.Signed ? 1 : 0))) - 1;
                    int q = (int)MathF.Round(e.Normalized ? c[k] * max : c[k]);
                    q = e.Signed ? Math.Clamp(q, -(1 << (b - 1)), (1 << (b - 1)) - 1) : Math.Clamp(q, 0, (1 << b) - 1);
                    w |= ((uint)q & ((1u << b) - 1)) << sh; sh += b;
                }
                BE.W32(g, o, w); break;
            }
            case VtxFormat.k_8_8_8_8:
            {
                float[] c = { v.X, v.Y, v.Z, v.W }; uint w = 0;
                for (int k = 0; k < 4; k++)
                {
                    int q = (int)MathF.Round(e.Normalized ? c[k] * (e.Signed ? 127f : 255f) : c[k]);
                    q = e.Signed ? Math.Clamp(q, -128, 127) : Math.Clamp(q, 0, 255);
                    w |= ((uint)q & 0xFF) << (8 * k);
                }
                BE.W32(g, o, w); break;
            }
        }
    }

    /// <summary>Uniform-grid nearest-neighbour lookup over the original vertex positions.</summary>
    sealed class NearestGrid
    {
        readonly Vector3[] _p; readonly Dictionary<(int, int, int), List<int>> _cells = new(); readonly float _cell;
        public NearestGrid(Vector3[] p)
        {
            _p = p;
            if (p.Length == 0) { _cell = 1; return; }
            var mn = p.Aggregate(Vector3.Min); var mx = p.Aggregate(Vector3.Max);
            var ext = mx - mn;
            _cell = MathF.Max(1e-3f, MathF.Cbrt(MathF.Max(1e-6f, ext.X * ext.Y * ext.Z) / Math.Max(1, p.Length)) * 2);
            for (int i = 0; i < p.Length; i++) { var k = Key(p[i]); if (!_cells.TryGetValue(k, out var l)) _cells[k] = l = new(); l.Add(i); }
        }
        (int, int, int) Key(Vector3 v) => ((int)MathF.Floor(v.X / _cell), (int)MathF.Floor(v.Y / _cell), (int)MathF.Floor(v.Z / _cell));
        public int Nearest(Vector3 q)
        {
            var (kx, ky, kz) = Key(q);
            for (int r = 0; r < 8; r++)
            {
                int best = -1; float bd = float.MaxValue;
                for (int x = kx - r; x <= kx + r; x++) for (int y = ky - r; y <= ky + r; y++) for (int z = kz - r; z <= kz + r; z++)
                {
                    if (Math.Max(Math.Abs(x - kx), Math.Max(Math.Abs(y - ky), Math.Abs(z - kz))) != r) continue;
                    if (!_cells.TryGetValue((x, y, z), out var l)) continue;
                    foreach (int i in l) { float dd = Vector3.DistanceSquared(_p[i], q); if (dd < bd) { bd = dd; best = i; } }
                }
                if (best >= 0) return best;
            }
            int b = 0; float bd2 = float.MaxValue;   // far away from everything: brute force
            for (int i = 0; i < _p.Length; i++) { float dd = Vector3.DistanceSquared(_p[i], q); if (dd < bd2) { bd2 = dd; b = i; } }
            return b;
        }
    }
}
