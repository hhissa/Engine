#pragma once
#include <resources/sdf_scene.h>

#include <QColor>
#include <QMainWindow>

class QListWidget;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QCheckBox;
class SceneViewport;

// A small Qt front-end over testbed/src/sdf_authoring.h's read/write/
// builder functions: pick a primitive type from a scrollable list, choose
// how it joins the scene so far (union/subtraction, plus a smoothness),
// pick a colour, set its transform, and add it -- building up an in-memory
// SdfScene one primitive at a time, then save it to an .sdf file the
// engine's load_sdf_scene()/GeometrySystem::load_scene() (or
// renderer_load_scene(), see renderer_frontend.h) can read straight back
// in. A live SceneViewport renders that same in-memory scene as you edit
// it (see sync_viewport_scene()), which needs actual compiled shaders/
// materials -- run this from bin/ (like testbed), NOT the repo root: the
// engine's own material/shader loading (MaterialSystem::material_path(),
// VulkanShaderModule) hardcodes "assets/..." relative to the process's
// working directory, and only bin/assets/ has compiled .spv shaders (via
// post-build.sh's `cp -R assets bin`) alongside the .kmt/.sdf files this
// tool reads/writes -- e.g. `cd bin && ../tools/sdf_editor/build/sdf_editor`.
// Paths this tool writes (assets/materials/, assets/scenes/) are relative
// to that same working directory, so both stay in sync with what the
// running engine is actually reading.
class SdfEditorWindow : public QMainWindow {
  Q_OBJECT

public:
  SdfEditorWindow();
  ~SdfEditorWindow() override;

private slots:
  void on_add_clicked();
  void on_remove_clicked();
  void on_pick_colour_clicked();
  // Connected to emissive_colour_button_'s clicked -- mirrors
  // on_pick_colour_clicked() for the emissive colour swatch (see
  // emissive_colour_/ensure_material()).
  void on_pick_emissive_colour_clicked();
  void on_pick_texture_clicked();
  void on_clear_texture_clicked();
  void on_save_clicked();
  void on_load_clicked();
  void on_type_selection_changed();
  void on_move_mode_clicked();
  void on_rotate_mode_clicked();
  // Connected to grid_button_'s toggled(bool) -- shows/hides the engine's
  // reference grid in the viewport (see SceneViewport::set_grid_visible()).
  // The grid is editor-only: the engine defaults it to hidden and only
  // this tool ever turns it on, so games never render it.
  void on_show_grid_toggled(bool checked);
  // Connected to viewport_'s primitive_picked(int) signal -- selects (or,
  // if layer_index is -1, clears selection of) the matching row in
  // contents_list_, keeping the side panel in sync with clicks made
  // directly in the 3D view (see ray_intersect.h/scene_viewport.cpp).
  void on_viewport_primitive_picked(int layer_index);
  // Connected to viewport_'s primitive_transformed(int, vec3, vec3, vec3)
  // signal, fired once when a gizmo drag ends -- writes the final
  // position/rotation/params back into scene_, refreshes the side panel's
  // fields to match, and calls sync_viewport_scene() to actually persist +
  // rebake (dragging itself never rebakes, see scene_viewport.h's class
  // comment).
  void on_viewport_primitive_transformed(int layer_index, glm::vec3 position,
                                        glm::vec3 rotation, glm::vec3 params);
  // Connected to contents_list_'s selection change -- the reverse
  // direction of on_viewport_primitive_picked(), so selecting a row here
  // also shows the gizmo on the matching primitive in the 3D view, and
  // populates the side panel's fields with that primitive's current values
  // (see populate_fields_from_selection()).
  void on_contents_list_selection_changed();
  // Connected to every "New Primitive" field's changed signal (operation,
  // smoothness, position, rotation, size) plus the colour/texture pickers.
  // If a primitive is currently selected, this reapplies the panel's
  // current values onto it live and rebakes -- the same fields double as
  // "what to create next" (nothing selected) and "live-edit the selection"
  // (something selected). No-op if nothing is selected, or if
  // populate_fields_from_selection() is what triggered the change (see
  // populating_fields_).
  void on_live_edit_changed();
  // Connected to every param_expr_edit_[i]'s textChanged. Refreshes which
  // param_spin_[i] are greyed out (a slot with a non-empty formula ignores
  // its spinbox) via update_field_enablement(), then behaves exactly like
  // on_live_edit_changed() otherwise.
  void on_param_expr_changed();

  // Mirrors on_type_selection_changed()/on_add_clicked()/
  // on_remove_clicked()/on_contents_list_selection_changed()/
  // on_live_edit_changed() above, for the Lights tab instead of Primitives
  // -- see populate_light_fields_from_selection()/apply_fields_to_light().
  void on_light_type_changed();
  void on_add_light_clicked();
  void on_remove_light_clicked();
  void on_pick_light_colour_clicked();
  void on_lights_list_selection_changed();
  void on_light_field_changed();
  // Connected to ambient_spin_'s valueChanged -- scene-wide, so unlike
  // every other field here it's applied immediately with no selection
  // check.
  void on_ambient_changed();

  // Mirrors on_type_selection_changed()/on_add_clicked()/on_remove_clicked()/
  // on_contents_list_selection_changed()/on_live_edit_changed() above, for
  // the Volumetrics tab instead of Primitives -- see
  // populate_volumetric_fields_from_selection()/apply_fields_to_volumetric().
  // Much closer to the Primitives tab than the Lights tab (a volumetric has
  // a full shape/material, not just colour+intensity), minus operation/
  // smoothness (a volumetric never joins a layer) and emissive/pixelation
  // (meaningless for something that's never a solid opaque surface), plus a
  // density field.
  void on_volumetric_type_changed();
  void on_add_volumetric_clicked();
  void on_remove_volumetric_clicked();
  void on_pick_volumetric_colour_clicked();
  void on_pick_volumetric_texture_clicked();
  void on_clear_volumetric_texture_clicked();
  void on_volumetrics_list_selection_changed();
  void on_volumetric_field_changed();

private:
  // Regenerates the "scene contents" list from scene_ (called after every
  // add/remove/load) and updates the window title's unsaved-changes marker.
  void refresh_contents_list();
  // Updates the position/rotation/param_spin_ fields' enabled/visible state
  // for the currently selected primitive type: position/rotation are
  // disabled (but stay visible) for Plane, which uses neither -- see
  // GeometryConfig::plane()/add_plane(); param_spin_/param_label_ show
  // exactly as many rows as the type's PrimitiveTypeSpec::params has,
  // relabeled to match (see type_spec_for() in the .cpp).
  void update_field_enablement();
  // Populates every "New Primitive" field (type, operation, smoothness,
  // position, rotation, per-type params, colour, texture) from
  // scene_.layers[layer_index]'s single primitive, so editing an existing
  // selection starts from its actual current values -- colour_/
  // texture_name_ are recovered by reading its material_name's .kmt file
  // back (see parse_material_file() in the .cpp), since material_name
  // alone doesn't say what colour/texture produced it. Sets
  // populating_fields_ around every setValue() call so the resulting
  // signals don't themselves trigger on_live_edit_changed().
  void populate_fields_from_selection(int layer_index);
  // The inverse of populate_fields_from_selection(): writes the panel's
  // current field values into scene_.layers[layer_index]'s operation/
  // smoothness/primitive, deriving a fresh material from colour_/
  // texture_name_ via ensure_material(). Does not call
  // sync_viewport_scene() itself -- callers do that once, after.
  void apply_fields_to_primitive(int layer_index);
  // Writes (or reuses, if already present) assets/materials/<name>.kmt for
  // colour_ (+ texture_name_, if not empty) and returns its name -- the
  // material_name every added/edited primitive references. Deterministic
  // from the colour's RGBA and texture name, so repeated colour/texture
  // choices reuse the same file instead of accumulating duplicates.
  std::string ensure_material() const;
  // Writes scene_ to a fixed on-disk path and (re-)loads it into the
  // renderer via renderer_clear_scenes()/renderer_load_scene(), so
  // viewport_'s next tick shows the current in-memory scene_ -- called
  // after every add/remove/load/live-edit. Simpler than tracking
  // per-primitive scene handles: this editor's entire authored world *is*
  // scene_, so clearing everything and reloading it whole is correct, not
  // just convenient.
  void sync_viewport_scene();

  // Regenerates the "Lights" list from scene_.lights -- mirrors
  // refresh_contents_list().
  void refresh_lights_list();
  // Mirrors populate_fields_from_selection()/apply_fields_to_primitive()
  // for a light instead of a primitive -- much simpler, since a light has
  // no material/texture to recover and only 4 fields total.
  void populate_light_fields_from_selection(int light_index);
  void apply_fields_to_light(int light_index);

  // Regenerates the "Volumetrics" list from scene_.volumetrics -- mirrors
  // refresh_contents_list().
  void refresh_volumetrics_list();
  // Updates volumetric_param_spin_/volumetric_param_label_'s enabled/visible
  // state for the currently selected type in volumetric_type_list_ --
  // mirrors update_field_enablement() (no formula fields here, though: a
  // volumetric's params are always plain constants).
  void update_volumetric_field_enablement();
  // Mirrors populate_fields_from_selection()/apply_fields_to_primitive() for
  // a volumetric instead of an opaque primitive.
  void populate_volumetric_fields_from_selection(int volumetric_index);
  void apply_fields_to_volumetric(int volumetric_index);
  // Mirrors ensure_material() -- writes/reuses assets/materials/<name>.kmt
  // for volumetric_colour_(+volumetric_texture_name_) and returns its name.
  // A separate helper (rather than reusing ensure_material()) since a
  // volumetric's material never has emissive/pixelation-exempt settings,
  // and uses its own independent colour_/texture_name_-equivalent state --
  // the Primitives tab's current selections shouldn't leak into whatever
  // volumetric is being added/edited alongside it.
  std::string ensure_volumetric_material() const;

  SdfScene scene_;
  QColor colour_ = Qt::white;
  std::string texture_name_; // empty => no diffuse map, colour_ only.
  // See Material::emissive_colour -- only takes effect once
  // emissive_intensity_spin_'s value is above 0 (the "off" default).
  QColor emissive_colour_ = Qt::white;

  // Monotonic sources for "layerN"/"lightN" names in on_add_clicked()/
  // on_add_light_clicked() -- never decremented, unlike scene_.layers.size()/
  // scene_.lights.size(), which repeat a previously-used value after a
  // removal (e.g. remove the 13th of 14 layers, then add a new one: size()
  // is 13 again, colliding with the surviving layer already named "layer13").
  // GeometrySystem::acquire() keys purely off this name
  // ("layerN/layerN_primitive"/"lightN"), so a collision doesn't create a
  // second entry -- it just bumps the existing one's reference count and
  // silently discards the new primitive's/light's position and parameters,
  // which is what made a freshly-added shape appear to not render at all.
  u64 next_layer_id_ = 0;
  u64 next_light_id_ = 0;
  u64 next_volumetric_id_ = 0;

  // Guards populate_fields_from_selection()'s setValue() calls against
  // re-entering on_live_edit_changed() -- without this, populating the
  // panel from a freshly-selected primitive would immediately "edit" it
  // right back (a harmless no-op in practice, but a wasted rebake per
  // field, and fragile if that ever stops being a no-op).
  bool populating_fields_ = false;

  QColor light_colour_ = Qt::white;
  // Mirrors populating_fields_, for populate_light_fields_from_selection()/
  // on_light_field_changed().
  bool populating_light_fields_ = false;

  QListWidget *type_list_ = nullptr;
  QListWidget *contents_list_ = nullptr;
  QComboBox *operation_combo_ = nullptr;
  QDoubleSpinBox *smoothness_spin_ = nullptr;
  QDoubleSpinBox *pos_x_ = nullptr;
  QDoubleSpinBox *pos_y_ = nullptr;
  QDoubleSpinBox *pos_z_ = nullptr;
  QDoubleSpinBox *rot_x_ = nullptr;
  QDoubleSpinBox *rot_y_ = nullptr;
  QDoubleSpinBox *rot_z_ = nullptr;
  // Generic per-type scalar parameters (radius, half-extents, corner
  // radius, ...) -- up to 4, labeled/shown per the current type's
  // PrimitiveTypeSpec (see type_spec_for() in the .cpp). Replaces a single
  // fixed "radius/half-extent" field now that primitive types need anywhere
  // from 1 (Sphere, Octahedron, Pyramid) to 4 (RoundBox, BoxFrame) numbers.
  QDoubleSpinBox *param_spin_[4] = {nullptr, nullptr, nullptr, nullptr};
  QLabel *param_label_[4] = {nullptr, nullptr, nullptr, nullptr};
  // "Parametric attribute" formula per slot (see
  // SdfPrimitiveDef::param_expressions/engine/src/resources/expression.h)
  // -- empty (the default) means that slot just uses param_spin_[i]'s
  // plain constant; non-empty text overrides it and disables the spinbox
  // to signal that (see update_field_enablement()).
  QLineEdit *param_expr_edit_[4] = {nullptr, nullptr, nullptr, nullptr};
  QPushButton *colour_button_ = nullptr;
  QPushButton *texture_button_ = nullptr;
  QPushButton *texture_clear_button_ = nullptr;
  QLabel *texture_label_ = nullptr;
  // World units per texture tile ("texture_scale=" in the written .kmt --
  // see Material::texture_scale engine-side). Applies to the default
  // checkerboard too, so it stays enabled even with no texture chosen.
  QDoubleSpinBox *texture_scale_spin_ = nullptr;
  QPushButton *emissive_colour_button_ = nullptr;
  QDoubleSpinBox *emissive_intensity_spin_ = nullptr; // 0 = not emissive
  QCheckBox *pixelation_exempt_check_ = nullptr; // see Material::pixelation_exempt
  QPushButton *move_mode_button_ = nullptr;
  QPushButton *rotate_mode_button_ = nullptr;
  QPushButton *grid_button_ = nullptr; // see on_show_grid_toggled()
  SceneViewport *viewport_ = nullptr;

  QListWidget *lights_list_ = nullptr;
  QComboBox *light_type_combo_ = nullptr;
  QLabel *light_vector_label_ = nullptr; // "Direction (x, y, z):" or
                                        // "Position (x, y, z):" -- see
                                        // on_light_type_changed().
  QDoubleSpinBox *light_vec_x_ = nullptr;
  QDoubleSpinBox *light_vec_y_ = nullptr;
  QDoubleSpinBox *light_vec_z_ = nullptr;
  QPushButton *light_colour_button_ = nullptr;
  QDoubleSpinBox *light_intensity_spin_ = nullptr;
  QDoubleSpinBox *ambient_spin_ = nullptr;

  // Volumetrics tab -- see the Volumetrics slot group above. Mirrors the
  // Primitives tab's own fields (type_list_/pos_*/rot_*/param_spin_/
  // param_label_/colour_button_/texture_button_/...), separately, since a
  // volumetric's "currently staged" values must be independent of whatever
  // opaque primitive the Primitives tab has staged/selected at the same
  // time.
  QListWidget *volumetric_type_list_ = nullptr;
  QListWidget *volumetrics_list_ = nullptr;
  QDoubleSpinBox *volumetric_pos_x_ = nullptr;
  QDoubleSpinBox *volumetric_pos_y_ = nullptr;
  QDoubleSpinBox *volumetric_pos_z_ = nullptr;
  QDoubleSpinBox *volumetric_rot_x_ = nullptr;
  QDoubleSpinBox *volumetric_rot_y_ = nullptr;
  QDoubleSpinBox *volumetric_rot_z_ = nullptr;
  QDoubleSpinBox *volumetric_param_spin_[4] = {nullptr, nullptr, nullptr, nullptr};
  QLabel *volumetric_param_label_[4] = {nullptr, nullptr, nullptr, nullptr};
  QColor volumetric_colour_ = Qt::white;
  QPushButton *volumetric_colour_button_ = nullptr;
  std::string volumetric_texture_name_; // empty => no diffuse map
  QPushButton *volumetric_texture_button_ = nullptr;
  QPushButton *volumetric_texture_clear_button_ = nullptr;
  QLabel *volumetric_texture_label_ = nullptr;
  QDoubleSpinBox *volumetric_texture_scale_spin_ = nullptr;
  // See SdfVolumetricDef::density.
  QDoubleSpinBox *volumetric_density_spin_ = nullptr;
  // Mirrors populating_fields_, for populate_volumetric_fields_from_
  // selection()/on_volumetric_field_changed().
  bool populating_volumetric_fields_ = false;
};
