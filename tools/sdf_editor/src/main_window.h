#pragma once
#include "scene_viewport.h" // for PrimitiveRef/GizmoTransformResult, used by
                            // slot signatures below
#include <renderer/renderer_types.inl> // for SceneHandle/kInvalidSceneHandle
#include <resources/sdf_scene.h>

#include <QColor>
#include <QMainWindow>

class QListWidget;
class QTreeWidgetItem;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QCheckBox;
class QTimer;
class ContentsTreeWidget;

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
  // Connected to new_layer_button_'s clicked -- creates an empty SdfLayerDef
  // (Union, smoothness 0, "layerN") with no primitives yet, adds it to the
  // tree, and selects it, so it becomes on_add_clicked()'s target (see
  // active_layer_index_). The explicit way to start a layer meant to hold
  // more than one primitive, rather than on_add_clicked()'s implicit "no
  // active layer -> make a fresh one" fallback.
  void on_new_layer_clicked();
  // Connected to copy_primitives_button_'s clicked -- deep-copies every
  // primitive contents_tree_'s current selection names into
  // primitive_clipboard_ (see its own comment). A selected primitive row
  // contributes itself; a selected LAYER row contributes every primitive in
  // it, so copying a whole layer's worth is still one click. Duplicates are
  // dropped (selecting a primitive and its layer row names it twice) and
  // the result is in ascending (layer, primitive) order. Enables
  // paste_primitives_button_. No-op (clipboard left as whatever it was) if
  // the selection names no primitives at all.
  void on_copy_primitives_clicked();
  // Connected to paste_primitives_button_'s clicked -- appends a deep copy
  // of every primitive in primitive_clipboard_ to the currently selected
  // layer (active_layer_index_ -- a selected layer row, or a selection all
  // within one layer), each with a freshly generated unique name (see
  // on_paste_primitives_clicked()'s own body for why that's essential, not
  // just tidy), then selects every newly pasted primitive row. With no
  // layer selected it starts a fresh one, mirroring on_add_clicked()'s
  // fallback, so the paste is never a silent no-op. No-op if
  // primitive_clipboard_ is empty (nothing copied yet -- the button stays
  // disabled in that case, but this guards direct calls too).
  void on_paste_primitives_clicked();
  void on_pick_colour_clicked();
  // Connected to emissive_colour_button_'s clicked -- mirrors
  // on_pick_colour_clicked() for the emissive colour swatch (see
  // emissive_colour_/ensure_material()).
  void on_pick_emissive_colour_clicked();
  void on_pick_texture_clicked();
  void on_clear_texture_clicked();
  // Mirrors on_pick_texture_clicked()/on_clear_texture_clicked() for
  // bump_map_name_ -- a SEPARATE texture from texture_name_ (see
  // Material::bump_map_name engine-side), sampled purely for surface-detail
  // perturbation, never for colour.
  void on_pick_bump_map_clicked();
  void on_clear_bump_map_clicked();
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

  // Switches the viewport between marching the chunked field and shading
  // its baked point cloud directly (see SceneViewport::
  // set_splat_visibility()).
  void on_splat_visibility_toggled(bool checked);
  // Connected to viewport_'s selection_changed(std::vector<PrimitiveRef>)
  // signal -- mirrors the given selection onto contents_tree_'s own item
  // selection, keeping the side panel in sync with clicks (and ctrl-clicks,
  // for multi-select) made directly in the 3D view (see
  // ray_intersect.h/scene_viewport.cpp).
  void on_viewport_selection_changed(std::vector<PrimitiveRef> selection);
  // Connected to viewport_'s
  // primitives_transformed(std::vector<GizmoTransformResult>) signal, fired
  // once when a gizmo drag ends -- writes every transformed primitive's
  // final position/rotation/params back into scene_, refreshes the side
  // panel's fields to match (only when exactly one primitive is selected),
  // and calls sync_viewport_scene() to actually persist + rebake (dragging
  // itself never rebakes, see scene_viewport.h's class comment).
  void on_viewport_primitives_transformed(std::vector<GizmoTransformResult> results);
  // --- Live gizmo drag. Hands the dragged primitive to the renderer as a
  // "dynamic" one for the duration, so it is drawn analytically instead of
  // from baked voxels and moving it re-bakes nothing -- see
  // renderer_set_dynamic_primitive(). Without this a drag re-voxelizes
  // every chunk its old and new bounds touch, on every mouse-move.
  void on_gizmo_drag_started(PrimitiveRef primitive);
  void on_gizmo_drag_moved(GizmoTransformResult transform);
  void on_gizmo_drag_ended();
  // The name the renderer knows a primitive by -- "scene<handle>/<layer>/
  // <primitive>", matching GeometrySystem::load_scene()'s prefixing. Empty
  // if the ref does not name a real primitive of the live scene.
  std::string renderer_primitive_name(PrimitiveRef ref) const;
  // Connected to contents_tree_'s itemSelectionChanged -- the reverse
  // direction of on_viewport_selection_changed(), so selecting row(s) here
  // also shows the gizmo on the matching primitive(s) in the 3D view.
  // Selecting exactly one primitive row also populates the side panel's
  // fields with its current values (see populate_fields_from_selection())
  // and live-editing becomes active; any other selection (zero, several, or
  // a layer row) leaves the fields as pure "staging" values for Add, same
  // as when nothing is selected. Also updates active_layer_index_ -- the
  // layer on_add_clicked() targets.
  void on_contents_tree_selection_changed();
  // Connected to contents_tree_'s primitives_reparented signal, fired after
  // a drag-and-drop moves one or more primitive rows under a different
  // layer row -- re-derives scene_.layers from the tree's now-current
  // structure (see sync_layers_from_tree()).
  void on_primitives_reparented();
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
  // Connected to repetition_combo_'s currentIndexChanged. Refreshes which
  // repeat_cell_*_/repeat_count_*_ spinboxes are enabled for the newly
  // chosen mode via update_field_enablement(), then behaves exactly like
  // on_live_edit_changed() otherwise.
  void on_repetition_mode_changed();

  // Mirrors on_type_selection_changed()/on_add_clicked()/
  // on_remove_clicked()/on_contents_tree_selection_changed()/
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
  // on_contents_tree_selection_changed()/on_live_edit_changed() above, for
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
  // Regenerates the "scene contents" tree from scene_ (called after every
  // add/remove/load/reparent) and updates the window title's unsaved-
  // changes marker -- one top-level item per layer (labeled with its
  // operation/smoothness), one child item per primitive within it. Layer
  // items store their scene_.layers[] index in kLayerIndexRole; primitive
  // items store their (layer_index, primitive_index) pair in
  // kLayerIndexRole/kPrimitiveIndexRole, plus the primitive's own stable
  // name in kPrimitiveNameRole (see sync_layers_from_tree(), which needs
  // that name to re-find a primitive after a drag-and-drop reparent).
  void refresh_contents_list();
  // Returns every currently-selected leaf (primitive) row in contents_tree_
  // as PrimitiveRefs -- ignores any selected layer row (a layer has nothing
  // to feed the gizmo/edit fields with). Used by
  // on_contents_tree_selection_changed() to drive viewport_'s selection.
  std::vector<PrimitiveRef> tree_selected_primitives() const;
  // Re-derives scene_.layers from contents_tree_'s current structure after
  // a drag-and-drop reparent: each layer keeps its own name/operation/
  // smoothness (read back from scene_.layers at its stored kLayerIndexRole
  // before rebuilding), but its primitives[] is rebuilt from whichever
  // primitive child items now sit under it in the tree, resolved back to
  // actual SdfPrimitiveDef values by the stable name every primitive item
  // carries (see refresh_contents_list()'s comment) -- position-based
  // (layer_index, primitive_index) pairs can't survive a reparent, since
  // that's exactly what just changed.
  void sync_layers_from_tree();
  // Updates the position/rotation/param_spin_ fields' enabled/visible state
  // for the currently selected primitive type: position/rotation are
  // disabled (but stay visible) for Plane, which uses neither -- see
  // GeometryConfig::plane()/add_plane(); param_spin_/param_label_ show
  // exactly as many rows as the type's PrimitiveTypeSpec::params has,
  // relabeled to match (see type_spec_for() in the .cpp).
  void update_field_enablement();
  // Populates every "New Primitive" field (type, operation, smoothness,
  // position, rotation, per-type params, colour, texture) from
  // scene_.layers[layer_index].primitives[primitive_index], so editing an
  // existing single-primitive selection starts from its actual current
  // values -- colour_/texture_name_ are recovered by reading its
  // material_name's .kmt file back (see parse_material_file() in the .cpp),
  // since material_name alone doesn't say what colour/texture produced it.
  // Sets populating_fields_ around every setValue() call so the resulting
  // signals don't themselves trigger on_live_edit_changed(). Only ever
  // called when exactly one primitive is selected -- see
  // on_contents_tree_selection_changed().
  void populate_fields_from_selection(int layer_index, int primitive_index);
  // The inverse of populate_fields_from_selection(): writes the panel's
  // current field values into scene_.layers[layer_index]'s operation/
  // smoothness and primitives[primitive_index], deriving a fresh material
  // from colour_/texture_name_ via ensure_material(). Does not call
  // sync_viewport_scene() itself -- callers do that once, after.
  void apply_fields_to_primitive(int layer_index, int primitive_index);
  // Writes (or reuses, if already present) assets/materials/<name>.kmt for
  // colour_ (+ texture_name_, if not empty) and returns its name -- the
  // material_name every added/edited primitive references. Deterministic
  // from the colour's RGBA and texture name, so repeated colour/texture
  // choices reuse the same file instead of accumulating duplicates.
  std::string ensure_material() const;
  // Writes scene_ to a fixed on-disk path and re-syncs it into the
  // renderer, so viewport_'s next tick shows the current in-memory scene_
  // -- called after every add/remove/load/live-edit. The very first call
  // (live_scene_handle_ still kInvalidSceneHandle) renderer_load_scene()s
  // it fresh; every call after that renderer_reconcile_scene()s against
  // the same handle instead -- touching only whatever primitive/light/
  // volumetric/layer actually changed since the last sync, rather than
  // releasing and re-registering this editor's entire authored world on
  // every single edit (see renderer_reconcile_scene()'s own comment for
  // exactly what that buys: an edit that only adds one primitive no
  // longer forces the chunked/streamed field to re-bake every chunk it
  // has resident, just the ones the new primitive actually touches).
  void sync_viewport_scene();
  // Debounced front door for sync_viewport_scene(), used only by the
  // rapid-fire live-edit handlers (on_live_edit_changed()/on_light_field_
  // changed()/on_ambient_changed()/on_volumetric_field_changed()) -- a
  // QDoubleSpinBox's valueChanged fires on every intermediate tick while
  // scrubbing/holding its arrows, each of which used to trigger its own
  // full save-to-disk + engine reconcile + chunk re-voxelize. Updates
  // viewport_'s own scene_ copy immediately (cheap, keeps the gizmo/click-
  // picking feeling instant) but defers the expensive part
  // (sync_viewport_scene_now()) behind sync_debounce_timer_, restarting it
  // on every call -- so a burst of edits within kSyncDebounceMs of each
  // other collapses into exactly one real sync, running kSyncDebounceMs
  // after the LAST one in the burst rather than once per edit. Every other
  // caller (add/remove/paste/load/drag-end/tree-reorder -- discrete,
  // deliberate, one-shot actions, not a rapid-fire source) still calls
  // sync_viewport_scene() directly for immediate, un-debounced feedback.
  void request_viewport_resync();
  // The actual save-to-disk + engine reconcile step, split out of sync_
  // viewport_scene() so request_viewport_resync()'s debounce timer has
  // something to call once it fires -- see both callers' own comments.
  void sync_viewport_scene_now();

  // Regenerates the "Lights" list from scene_.lights -- mirrors
  // refresh_contents_list().
  void refresh_lights_list();
  // Mirrors populate_fields_from_selection()/apply_fields_to_primitive()
  // for a light instead of a primitive -- much simpler, since a light has
  // no material/texture to recover and only 4 fields total.
  void populate_light_fields_from_selection(int light_index);
  void apply_fields_to_light(int light_index);
  // Shows/hides the viewport's gizmo on light_index: a Point light gets a
  // single-entry selection (PrimitiveRef::light_index) so its position can
  // be dragged, same as a primitive; a Directional light has no position
  // for a gizmo to show at all, so this just clears the selection instead.
  // Called whenever lights_list_'s current row changes and whenever a
  // selected light's own Type field flips between the two (see
  // on_lights_list_selection_changed()/on_light_field_changed()).
  void update_viewport_light_selection(int light_index);

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
  // The renderer's handle for scene_'s live-preview registration -- see
  // sync_viewport_scene(). kInvalidSceneHandle until the very first
  // sync_viewport_scene() call (renderer_load_scene()'s first-time
  // registration); every call after that reconciles against this same
  // handle instead of loading a fresh one.
  SceneHandle live_scene_handle_ = kInvalidSceneHandle;
  // Whether the drag that just ended had a dynamic primitive behind it --
  // i.e. whether on_gizmo_drag_started() actually handed the renderer a
  // name. Only a lone non-light selection qualifies (the renderer tracks
  // exactly one, and a light has no baked geometry), so a ctrl-click
  // multi-selection and a light drag both leave this false.
  //
  // on_viewport_primitives_transformed() skips the reconcile on release
  // because dropping the dynamic primitive is what queues the re-bake. With
  // no dynamic primitive there is nothing to drop and nothing queued, so
  // that shortcut would leave the move written to the primitive buffer but
  // absent from the baked field -- and the splat pass, which is what draws
  // the image whenever no primitive is dynamic, reads only the baked field.
  // The primitives simply would not appear to move.
  bool drag_had_dynamic_primitive_ = false;
  // Debounces request_viewport_resync() -- see its own comment. Single-
  // shot, (re)started on every call rather than left running, so it fires
  // exactly once kSyncDebounceMs after the LAST edit in a burst.
  QTimer *sync_debounce_timer_ = nullptr;
  QColor colour_ = Qt::white;
  std::string texture_name_; // empty => no diffuse map, colour_ only.
  // Separate from texture_name_ above -- empty => no bump map, no bump
  // mapping applied at all (see Material::bump_map_name engine-side).
  std::string bump_map_name_;
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
  // Names every added primitive "primitiveN", independent of which layer it
  // lands in -- now that on_add_clicked() can add more than one primitive
  // into the same layer (see active_layer_index_), the old
  // "<layer_name>_primitive" convention (exactly one per layer) would
  // collide the moment a second primitive joined a layer.
  u64 next_primitive_id_ = 0;

  // Guards populate_fields_from_selection()'s setValue() calls against
  // re-entering on_live_edit_changed() -- without this, populating the
  // panel from a freshly-selected primitive would immediately "edit" it
  // right back (a harmless no-op in practice, but a wasted rebake per
  // field, and fragile if that ever stops being a no-op).
  bool populating_fields_ = false;

  // Which scene_.layers[] entry on_add_clicked() pushes a new primitive
  // into, instead of creating a fresh layer -- kept in sync with
  // contents_tree_'s current selection by
  // on_contents_tree_selection_changed() (a primitive row's parent layer,
  // or a layer row itself; -1 if the selection doesn't imply one, e.g.
  // nothing selected or several primitives spanning different layers).
  int active_layer_index_ = -1;

  QColor light_colour_ = Qt::white;
  // Mirrors populating_fields_, for populate_light_fields_from_selection()/
  // on_light_field_changed().
  bool populating_light_fields_ = false;

  QListWidget *type_list_ = nullptr;
  ContentsTreeWidget *contents_tree_ = nullptr;
  QPushButton *new_layer_button_ = nullptr;
  QPushButton *copy_primitives_button_ = nullptr;
  QPushButton *paste_primitives_button_ = nullptr;
  // Set by on_copy_primitives_clicked(), read by
  // on_paste_primitives_clicked() -- empty means "nothing copied yet"
  // (paste_primitives_button_ stays disabled the whole time it is). One
  // entry per primitive the selection named, in ascending (layer,
  // primitive) index order at copy time. Plain values, not pointers/indices
  // into scene_: copying takes a snapshot independent of whatever
  // scene_.layers does afterward (edits, removals, even loading an entirely
  // different scene), exactly what a clipboard should survive.
  //
  // Deliberately primitives rather than whole layers: the layer a paste
  // lands in is chosen at PASTE time (the selected one), so the copy has no
  // business carrying an operation/smoothness of its own -- pasting into a
  // subtraction layer should subtract.
  std::vector<SdfPrimitiveDef> primitive_clipboard_;
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
  // Domain repetition (see SdfPrimitiveDef::repetition_mode) -- combo order
  // matches SdfRepetitionMode's own enum order exactly (None/Infinite/
  // Limited/Rotational/Rectangular), same "row index == enum value"
  // convention type_list_ uses. repeat_cell_*_/repeat_count_*_ are shared
  // across every mode (see update_field_enablement() for which ones are
  // enabled per mode) rather than a separate field set per mode, since at
  // most 3 of each are ever needed.
  QComboBox *repetition_combo_ = nullptr;
  QDoubleSpinBox *repeat_cell_x_ = nullptr;
  QDoubleSpinBox *repeat_cell_y_ = nullptr;
  QDoubleSpinBox *repeat_cell_z_ = nullptr;
  QDoubleSpinBox *repeat_count_x_ = nullptr;
  QDoubleSpinBox *repeat_count_y_ = nullptr;
  QDoubleSpinBox *repeat_count_z_ = nullptr;
  // Domain deformation (see SdfPrimitiveDef::twist/bend/displace_amplitude/
  // displace_frequency) -- all default to their identity/no-op value
  // (0, matching the struct default, except displace_frequency_spin_
  // which defaults to 20 the same way the struct does).
  QDoubleSpinBox *twist_spin_ = nullptr;
  QDoubleSpinBox *bend_spin_ = nullptr;
  QDoubleSpinBox *displace_amplitude_spin_ = nullptr;
  QDoubleSpinBox *displace_frequency_spin_ = nullptr;
  QPushButton *colour_button_ = nullptr;
  QPushButton *texture_button_ = nullptr;
  QPushButton *texture_clear_button_ = nullptr;
  QLabel *texture_label_ = nullptr;
  // Separate texture picker for bump_map_name_ -- mirrors texture_button_/
  // texture_clear_button_/texture_label_ exactly.
  QPushButton *bump_map_button_ = nullptr;
  QPushButton *bump_map_clear_button_ = nullptr;
  QLabel *bump_map_label_ = nullptr;
  // World units per texture tile ("texture_scale=" in the written .kmt --
  // see Material::texture_scale engine-side). Applies to the default
  // checkerboard too, so it stays enabled even with no texture chosen.
  QDoubleSpinBox *texture_scale_spin_ = nullptr;
  // World-unit texture translate ("texture_offset=" in the written .kmt --
  // see Material::texture_offset engine-side) -- shifts the triplanar
  // pattern along each world axis. Applies to the default checkerboard
  // too, same as texture_scale_spin_.
  QDoubleSpinBox *texture_offset_x_ = nullptr;
  QDoubleSpinBox *texture_offset_y_ = nullptr;
  QDoubleSpinBox *texture_offset_z_ = nullptr;
  // Texture rotate, in degrees in this UI ("texture_rotation=" in the
  // written .kmt is radians -- see Material::texture_rotation engine-side;
  // ensure_material()/populate_fields_from_selection() do the conversion,
  // same as rot_x_/rot_y_/rot_z_ do for a primitive's own rotation).
  QDoubleSpinBox *texture_rotation_spin_ = nullptr;
  QPushButton *emissive_colour_button_ = nullptr;
  QDoubleSpinBox *emissive_intensity_spin_ = nullptr; // 0 = not emissive
  QCheckBox *pixelation_exempt_check_ = nullptr; // see Material::pixelation_exempt
  QPushButton *move_mode_button_ = nullptr;
  QPushButton *rotate_mode_button_ = nullptr;
  QPushButton *grid_button_ = nullptr; // see on_show_grid_toggled()
  QPushButton *splat_button_ = nullptr; // see on_splat_visibility_toggled()
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
  // Mirror texture_offset_x_/y_/z_ and texture_rotation_spin_ above, for
  // the Volumetrics tab's own independently-staged material.
  QDoubleSpinBox *volumetric_texture_offset_x_ = nullptr;
  QDoubleSpinBox *volumetric_texture_offset_y_ = nullptr;
  QDoubleSpinBox *volumetric_texture_offset_z_ = nullptr;
  QDoubleSpinBox *volumetric_texture_rotation_spin_ = nullptr;
  // See SdfVolumetricDef::density.
  QDoubleSpinBox *volumetric_density_spin_ = nullptr;
  // Mirrors populating_fields_, for populate_volumetric_fields_from_
  // selection()/on_volumetric_field_changed().
  bool populating_volumetric_fields_ = false;
};
