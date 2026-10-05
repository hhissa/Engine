#pragma once
#include "../defines.h"

#include <glm/glm.hpp>
#include <iosfwd>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The authored VALUES of a material, separated from both its runtime
// representation (Material, material_system.h -- which additionally holds
// resolved VulkanTexture pointers and shader instance state) and from
// whatever key happens to identify it to the renderer.
//
// That separation is the whole point of this file, so it is worth stating
// plainly. Before it existed, the sdf_editor encoded every material
// property into the material's own FILENAME
// ("qt_colour_dddbbeff_ts060_white") and wrote a .kmt under that name.
// The name was not a label for a material, it *was* the material,
// serialised -- which meant:
//
//   * A material could not be edited. Changing a colour did not modify
//     anything; it computed a different name and pointed the primitive at
//     a different file. "Make every oak surface darker" was unexpressible
//     because there was no oak, only 35 primitives that coincidentally
//     shared a value tuple.
//   * A material could not be renamed, for the same reason.
//   * Every new property cost a bespoke snprintf format, a quantisation
//     rule, a "skip this suffix when it is at its default so old names
//     keep working" guard, and a matching branch in three separate
//     parsers.
//   * Files accumulated forever: 949 .kmt files existed for the 93
//     materials actually referenced by any scene.
//
// The fix is to split the three things that name was doing at once:
//
//   1. DEFINITION -- MaterialDef below. Identified by `id` (stable, opaque,
//      never derived from the values) and labelled by `display_name`
//      (free-form, renameable). Lives in the scene file.
//   2. BINDING -- which primitive uses which definition, plus any sparse
//      per-primitive overrides. Lives on SdfPrimitiveDef (sdf_scene.h).
//   3. PACKING -- the key the renderer deduplicates by. That one IS
//      content-addressed, correctly: see material_def_content_key() below,
//      which is what MaterialSystem::acquire_def() keys its cache on. The
//      old design's mistake was not content-addressing, it was promoting
//      the content address to the authoring identity.
//
// Adding a property means adding a field here and ONE line to
// for_each_material_property() below. The reader, the writer, the content
// key, the change classifier, and (in the editor) the property panel all
// derive from that single declaration.

// Stable, opaque authoring identity. Deliberately NOT derived from the
// material's values -- that is exactly the mistake this file exists to
// undo -- so two materials that happen to be identical are still two
// materials if the author made two, and a material that is edited keeps
// its id (and therefore every primitive that references it).
//
// Serialised as 16 lowercase hex digits (see material_id_to_string()).
using MaterialId = u64;
inline constexpr MaterialId kInvalidMaterialId = 0;

// What a changed property invalidates -- the classification that lets the
// editor apply a live edit at the right cost instead of the worst one.
//
// Ordered by severity so a diff over several properties can just take the
// max (see material_def_diff() below).
enum class MaterialDirty : u32 {
  // Nothing at all changed.
  None = 0,
  // Shading only: the renderer re-uploads this material's entry and that
  // is the whole cost. No re-bake, no cache invalidation, no queue drain.
  //
  // This is the common case and it is genuinely this cheap: bricks store a
  // per-cell PRIMITIVE index and the chunk cache keys hash the candidate
  // list plus primitive indices, so no material value can possibly
  // invalidate baked geometry. (See [[deterministic-scene-order]] for the
  // hashing rule this relies on staying true.)
  Shading = 1,
  // Emissive changes register/unregister a synthesised point light (see
  // VulkanRaymarchShader::rebuild_static_scene()) and therefore invalidate
  // the GI bake. The only genuinely expensive material edit, and the
  // reason this enum exists rather than everything being assumed cheap.
  Lighting = 2,
};

// One sparse per-primitive override of a single MaterialDef property.
//
// The alternative to overrides is forking: an author who wants one drawer
// front slightly darker than the rest of the oak duplicates the material,
// and the library fragments. That is precisely how 949 files came to
// exist, so the ability to say "this material, but with base_colour
// changed" is not a luxury feature -- it is what keeps the library small.
//
// Stored as the property's key and its value in the same textual form the
// scene file uses, so it needs no per-type variant and rides through
// material_def_set_property() exactly like a parsed line does.
struct MaterialOverride {
  std::string key;
  std::string value;
};

struct MaterialDef {
  // See MaterialId above. kInvalidMaterialId means "not a library
  // material" -- e.g. a def synthesised from a legacy .kmt file that has
  // not been imported into a scene yet.
  MaterialId id = kInvalidMaterialId;
  // A label, not a key. Renaming this changes nothing else.
  std::string display_name;

  // --- Base ---------------------------------------------------------
  glm::vec4 base_colour{1.0f};
  // Diffuse texture name, resolved through TextureSystem. Empty means no
  // map: the runtime falls back to the default (checkerboard) texture
  // tinted by base_colour, same as it always did.
  std::string base_map;
  // World units one full repeat of the texture spans. Matches the old
  // Material::texture_scale default exactly so imported materials look
  // identical.
  f32 uv_scale = 0.6f;
  // World-space offset applied BEFORE the uv_scale divide, i.e. authored
  // in world units, and a per-plane UV rotation in radians.
  glm::vec3 uv_offset{0.0f};
  f32 uv_rotation = 0.0f;

  // --- Surface ------------------------------------------------------
  // A SEPARATE texture from base_map, sampled for luminance only. Empty
  // means no bump mapping at all (the runtime substitutes a genuinely
  // uniform texture, so the perturbation comes out to zero).
  std::string bump_map;
  f32 bump_strength = 1.0f;
  // Dielectric surface roughness, 0 (mirror) to 1 (fully diffuse). Not
  // yet read by any shader -- the deferred pass is currently pure Lambert
  // with no specular lobe at all -- but declared here because it is the
  // input every one of glass, plastic and still liquid needs, and because
  // the point of this file is that adding it later costs one line.
  f32 roughness = 0.5f;

  // --- Emission -----------------------------------------------------
  glm::vec3 emissive_colour{1.0f};
  // 0 (the default) means not emissive. Above 0 makes any primitive using
  // this material a visible light source AND registers a synthesised
  // point light for it -- which is why this property is classified
  // MaterialDirty::Lighting and everything above is Shading.
  f32 emissive_intensity = 0.0f;

  // --- Transmission -------------------------------------------------
  // Index of refraction. 0 (the default) means opaque -- NOT 1.0, so that
  // "has this material been authored as transmissive at all" is a single
  // comparison rather than a float epsilon test against air.
  //
  // Otherwise the meaningful band is [1, ~3]. A value in (0, 1) is NOT a
  // weakly-refracting glass, it describes light leaving a denser medium,
  // and at an air interface it renders as a near-perfect mirror by two
  // independent routes: F0 approaches 1 as the IOR approaches 0, and
  // eta = 1/ior total-internal-reflects every ray. The renderer clamps it
  // (with a warning) rather than drawing that, and the editor exposes a
  // Transmissive toggle plus a [1, 3] spin box rather than one continuous
  // 0-to-3 control that walks straight through the bad band.
  f32 ior = 0.0f;
  // Beer-Lambert absorption, authored the way an artist can actually
  // judge it: the colour a slab of absorption_ref_thickness world units
  // transmits. The renderer solves this to a per-channel coefficient
  // (sigma = -ln(tint) / ref_thickness) rather than making anyone author
  // reciprocal metres.
  glm::vec3 absorption_tint{1.0f};
  f32 absorption_ref_thickness = 0.1f;
  // Parallel-walled glass thin enough that refraction displaces the ray by
  // less than a pixel -- a windowpane, a bottle wall. Lets the renderer
  // skip the interior march entirely, which is both cheaper AND more
  // accurate there than simulating a sub-pixel displacement.
  bool thin_walled = false;

  // --- Flags --------------------------------------------------------
  // False means the primitive is still drawn and lit normally but every
  // shadow ray passes straight through it.
  bool casts_shadow = true;
  // Excludes the primitive from the screen-space pixelation post-process.
  bool pixelation_exempt = false;

  // Keys this build did not recognise, preserved verbatim so a scene
  // written by a newer editor round-trips through an older one without
  // silently losing data. Never participates in the content key or the
  // diff -- an unknown key cannot be known to affect rendering, and
  // treating it as if it did would make every load of a
  // forward-versioned scene look like an edit.
  std::vector<std::pair<std::string, std::string>> unknown_keys;
};

// THE property table. Every consumer -- reader, writer, content key,
// change classifier, GPU packer, editor panel -- walks this, so a property
// is declared exactly once in the whole codebase.
//
// Templated on the def reference type so the same walk serves both a
// mutable MaterialDef (parsing, editing) and a const one (writing,
// hashing) without a second copy that could drift out of sync.
//
// `visit` is called as visit(key, member_reference, dirty_class). It is
// expected to overload on the member's type -- the five that occur are
// f32, bool, glm::vec3, glm::vec4 and std::string.
template <typename Def, typename Visitor>
void for_each_material_property(Def &def, Visitor &&visit) {
  visit("base_colour", def.base_colour, MaterialDirty::Shading);
  visit("base_map", def.base_map, MaterialDirty::Shading);
  visit("uv_scale", def.uv_scale, MaterialDirty::Shading);
  visit("uv_offset", def.uv_offset, MaterialDirty::Shading);
  visit("uv_rotation", def.uv_rotation, MaterialDirty::Shading);

  visit("bump_map", def.bump_map, MaterialDirty::Shading);
  visit("bump_strength", def.bump_strength, MaterialDirty::Shading);
  visit("roughness", def.roughness, MaterialDirty::Shading);

  visit("emissive_colour", def.emissive_colour, MaterialDirty::Lighting);
  visit("emissive_intensity", def.emissive_intensity, MaterialDirty::Lighting);

  visit("ior", def.ior, MaterialDirty::Shading);
  visit("absorption_tint", def.absorption_tint, MaterialDirty::Shading);
  visit("absorption_ref_thickness", def.absorption_ref_thickness,
        MaterialDirty::Shading);
  visit("thin_walled", def.thin_walled, MaterialDirty::Shading);

  visit("casts_shadow", def.casts_shadow, MaterialDirty::Shading);
  visit("pixelation_exempt", def.pixelation_exempt, MaterialDirty::Shading);
}

// --- Identity -------------------------------------------------------

// 16 lowercase hex digits. kInvalidMaterialId formats as all zeroes, which
// is also what a missing/malformed id parses back to, so a round trip
// through a hand-edited file degrades to "no material" rather than to a
// random one.
std::string material_id_to_string(MaterialId id);
bool material_id_from_string(std::string_view text, MaterialId &out);

// A fresh id, distinct from every id this process has already handed out
// and (with overwhelming probability) from every id in any scene file.
// Random rather than sequential precisely so that two scenes authored
// independently can be merged without their ids colliding.
MaterialId material_id_generate();

// --- Serialisation ---------------------------------------------------

// Sets one property by its table key. Returns false if the key is not a
// known property (the caller decides whether that is a warning or an
// unknown_keys entry) or if the value fails to parse as that property's
// type. A failed parse leaves the property at whatever it already held.
bool material_def_set_property(MaterialDef &def, std::string_view key,
                              const std::string &value);

// Formats one property by its table key into `out`. Returns false for an
// unknown key. Used by the editor's override UI to capture a property's
// current value in the same textual form the file uses.
bool material_def_get_property(const MaterialDef &def, std::string_view key,
                              std::string &out);

// Writes `id=` and every property whose value differs from a
// default-constructed MaterialDef, each on its own "key=value" line
// prefixed by `indent`, followed by any preserved unknown keys.
//
// display_name is deliberately NOT written: every container that persists
// a def already has a natural place for its label -- a .sdf material block
// writes it in the block header, exactly as "layer NAME {" and
// "light NAME {" already do -- and writing it twice would invite the two
// copies to drift.
//
// Omitting defaults is not cosmetic. It is the rule the old filename
// scheme re-implemented by hand for every single property ("only append
// this suffix when it is non-default, so materials authored before this
// feature existed keep their old names"), stated once and applied
// uniformly -- which is what makes adding a property a one-line change
// instead of a compatibility exercise.
void material_def_write(const MaterialDef &def, std::ostream &out,
                       std::string_view indent);

// --- Comparison ------------------------------------------------------

// The strongest invalidation implied by the difference between two defs,
// or MaterialDirty::None if their values are identical. Ignores `id`,
// `display_name` and `unknown_keys`: renaming a material is not a
// rendering change, and neither is carrying a key this build cannot
// interpret.
MaterialDirty material_def_diff(const MaterialDef &a, const MaterialDef &b);

// The renderer's deduplication key: a deterministic string built from
// every property VALUE, excluding id, display_name and unknown_keys.
//
// Two defs that would render identically produce the same key and share
// one runtime Material (and, once the GPU material table lands, one slot)
// no matter how they were authored -- which is the correct place for
// content addressing, as opposed to the filename.
//
// Returned as a full string rather than a hash so that a collision is
// impossible by construction rather than merely improbable; this is an
// unordered_map key, never a filename, so its length costs nothing.
std::string material_def_content_key(const MaterialDef &def);

// --- Overrides -------------------------------------------------------

// Applies each override in order, ignoring (with a warning) any whose key
// is not a known property. Returns the def with overrides folded in --
// which is what the renderer actually resolves and what
// material_def_content_key() is then taken over, so two primitives that
// override a shared material identically still share one runtime entry.
MaterialDef material_def_apply_overrides(
    const MaterialDef &def, const std::vector<MaterialOverride> &overrides);

// The IOR the renderer should actually use for this material: 0 if it is
// opaque, otherwise the authored value clamped up to 1.
//
// One function rather than a clamp at each use site, because the rule is
// subtle enough to get wrong twice: a value in (0, 1) is not weak glass,
// it renders as a near-perfect mirror (see MaterialDef::ior). `clamped`
// reports whether the value had to be corrected, so a caller that can warn
// does.
f32 material_effective_ior(const MaterialDef &def, bool &clamped);
inline f32 material_effective_ior(const MaterialDef &def) {
  bool ignored = false;
  return material_effective_ior(def, ignored);
}

// --- Legacy import ---------------------------------------------------

// Parses assets/materials/<name>.kmt into a MaterialDef, mapping the old
// key names onto the new fields (diffuse_colour -> base_colour,
// texture_scale -> uv_scale, diffuse_map_name -> base_map, and so on).
// Returns false if the file does not exist.
//
// This is the ONLY .kmt parser in the codebase. MaterialSystem::acquire()
// goes through it, and so does scene import -- previously there were
// three near-identical copies (material_system.cpp, the sdf_editor's
// parse_material_file(), and the editor's own inverse encoder), which is
// how the editor's quantisation and the engine's parse were able to
// disagree about a value without anything noticing.
//
// `display_name` is left empty: a legacy file's name is a value encoding,
// not a label, so the importer picks a real one instead (see
// material_def_suggest_name()).
bool material_def_load_kmt(std::string_view name, MaterialDef &out);

// A human-usable initial name for an imported legacy material, derived
// from what it actually looks like: the texture name if it has one
// ("officecarpet"), qualified by the tint when that tint is not white
// ("officecarpet (aa5500)"), falling back to the colour alone. Placeholder
// names an author will rename -- but ones that hint at the content, which
// "Material 17" does not.
std::string material_def_suggest_name(const MaterialDef &def);
