#pragma once
#include "sculpt_document.h"

#include <QColor>
#include <QMainWindow>

class SceneViewport;
class QLabel;
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
class QPushButton;
class QListWidget;

// M2a's top-level window: a File menu (Save/Load, Ctrl+S/Ctrl+O), an Edit
// menu (Undo/Redo, Ctrl+Z/Ctrl+Shift+Z -- M2b), and a brush panel (shape/
// op/smoothness/size, see build_brush_panel()) over an embedded
// SceneViewport, plus a status label showing how many stamps are in the
// current document. No materials tab, no gizmo, no edit-history UI -- all
// explicitly out of scope for this milestone (see the plan's M2a/M2b
// sections -- Move/Delete-of-an-arbitrary-edit is a later milestone,
// rotation is deferred further still). Mirrors tools/sdf_editor's
// SdfEditorWindow (git history) only in its very top-level shape
// (createWindowContainer + a menu bar); none of its CSG-specific widget
// tree (contents tree, materials tab, property spinboxes) is reused.
class ModellingWindow : public QMainWindow {
  Q_OBJECT

public:
  ModellingWindow();
  ~ModellingWindow() override;

private slots:
  void on_stamp_placed(glm::vec3 center, SculptBrushShape shape_type,
                       glm::vec4 params, f32 smoothness, SculptBrushOp op,
                       int material_slot, f32 looseness);
  void on_save_triggered();
  void on_load_triggered();
  void on_undo_triggered();
  void on_redo_triggered();
  void on_stroke_ended(int stamp_count);
  void on_shape_changed(int index);
  void on_op_changed(int index);
  void on_smoothness_changed(double value);
  void on_size_changed(double value);
  // M3 (Looseness) -- see build_brush_panel()/looseness_spin_'s own
  // comments.
  void on_looseness_changed(double value);

  // M4a (Stretch) -- see build_brush_panel()/stretch_x_spin_'s own
  // comments.
  void on_stretch_x_changed(double value);
  void on_stretch_y_changed(double value);
  void on_stretch_z_changed(double value);

  // M5 (Finishes) -- see build_brush_panel()/finish_combo_/material_
  // colour_button_'s own comments and apply_finish_and_colour() below.
  // Both handlers funnel into apply_finish_and_colour(): picking a new
  // Finish preset or a new base colour each allocate a FRESH material
  // slot for subsequent stamps, same as every other brush-panel control
  // only affecting what gets stamped NEXT, never retroactively.
  void on_finish_changed(int index);
  void on_pick_material_colour_clicked();

  // M6 (Flecks) -- see build_brush_panel()/fleck_combo_/fleck_strength_
  // spin_'s own comments. Same "funnel into apply_finish_and_colour()"
  // shape as the Finish handlers directly above: a material slot now
  // carries its Finish AND its Fleck together, allocated by the one
  // shared call, not two independent paths.
  void on_fleck_changed(int index);
  void on_fleck_strength_changed(double value);

  // M3b (Clone).
  void on_clone_mode_toggled(bool checked);
  void on_set_clone_source_clicked();
  void on_clone_source_missing();

  // M3c (Spraypaint).
  void on_spray_mode_toggled(bool checked);
  void on_spray_opacity_changed(double value);

  // M4a (Move).
  void on_move_mode_toggled(bool checked);
  void on_move_source_missing();

  // M4b (Delete-of-an-arbitrary-edit).
  void on_delete_edit_clicked();

  // M4c (Move-of-an-arbitrary-edit): "Move Selected Edit" arms
  // SceneViewport::begin_move_selected_edit() for whatever row is currently
  // selected -- it does NOT itself touch document_/the engine (unlike
  // Delete, nothing is removed, so there is no undo_group_sizes_ bookkeeping
  // to shrink either: the edit stays exactly where it is, at the same list
  // position, until a later drag's release actually commits a new center).
  // on_edit_moved() is that later commit's handler -- connected to
  // SceneViewport::edit_moved(), it updates document_'s matching record
  // in place once the engine has confirmed the move happened.
  void on_move_edit_clicked();
  void on_edit_moved(int index, glm::vec3 new_center);

  // M4 (Crop/Cutout) -- see build_brush_panel()'s own additions and
  // SceneViewport::do_crop()/do_cutout()/load_cutout_file()'s own
  // comments. Deliberately NOT wired into document_/undo_group_sizes_/
  // redo_groups_ or history_list_ -- Crop/Cutout are whole-volume filters
  // outside the replayable-stamp model this window's save/load and Edit
  // History machinery is built around (see the engine-side header comment
  // on VulkanRaymarchShader::crop_volume() for the full documented scope
  // decision); this is simply not a kind of entry those lists know how to
  // hold.
  void on_crop_clicked();
  void on_cutout_clicked();
  void on_open_cutout_triggered();

private:
  void update_status_label();

  // Builds the brush panel dock (shape dropdown, op toggle, size/
  // smoothness spin boxes, M3b's Clone controls) and wires it to
  // viewport_'s set_brush_*()/set_clone_mode()/capture_clone_source().
  // Called once from the constructor.
  void build_brush_panel();

  // M5 (Finishes) + M6 (Flecks): allocates a FRESH material slot (next_
  // material_slot_, post-incremented -- see its own comment) and
  // configures it via viewport_->configure_material() from whatever
  // finish_combo_/current_material_colour_ AND fleck_combo_/fleck_
  // strength_spin_ currently hold, then points viewport_'s brush_
  // material_slot_ at it (set_brush_material_slot()) so the NEXT stamp/
  // clone-capture picks it up. Called from the Finish handlers AND the
  // Fleck handlers below -- any one of the four controls changing is "a
  // new material", in this tool's model, and ALL FOUR always go into the
  // same fresh slot together (one slot carries both a Finish and a Fleck,
  // never two independent allocation paths). Also refreshes material_
  // colour_button_'s background swatch so the button itself shows the
  // active colour. fleck_strength_spin_ at its startup default of 0 means
  // a session that never touches the Fleck controls allocates slots
  // exactly as M5 did -- see fleck_strength_spin_'s own comment.
  void apply_finish_and_colour();

  // M3b: refreshes clone_source_label_'s text from viewport_'s current
  // clone_source_shape()/clone_source_size()/clone_source_op() -- called
  // right after a capture and once at startup (so the label never shows a
  // stale "none captured" placeholder is wrong, it just always reflects
  // whatever's actually in SceneViewport's clone_source_ right now).
  void update_clone_source_label();

  // M4a (Move): installed on viewport_ as its MovePickupFn (see
  // SceneViewport::set_move_pickup_callback()'s own comment) -- undoes the
  // engine's top stamp, retracts document_'s matching tail record, and
  // keeps undo_group_sizes_/redo_groups_ honest about it (see this
  // method's own .cpp comment for the full bookkeeping reasoning: a pickup
  // is mechanically an Undo of exactly one stamp, so it is handled the
  // same way on_undo_triggered() handles an Undo, just for a single stamp
  // rather than a whole stroke-group). Returns std::nullopt if there is
  // nothing to pick up (document_ empty, or the engine-side undo call
  // unexpectedly fails) -- SceneViewport::mousePressEvent() turns that into
  // the move_source_missing() status message.
  std::optional<SculptStampRecord> pickup_for_move();

  // M4b: builds the "Edit History" dock (a plain list -- index/shape/
  // rounded position, see refresh_edit_history_list() -- plus a "Delete
  // Selected Edit" button) over document_.stamps(). Called once from the
  // constructor, alongside build_brush_panel().
  void build_edit_history_panel();

  // M4b: repopulates history_list_ from document_.stamps() -- called from
  // update_status_label() (see its own comment) so the list and the status
  // bar's stamp count always refresh together, at every point the existing
  // milestones already kept that count current (a fresh stamp, Undo, Redo,
  // Load, and now Delete). Preserves the current row across a refresh that
  // doesn't shrink the list out from under it, so selecting a row to delete
  // doesn't get silently clobbered by an unrelated refresh.
  void refresh_edit_history_list();

  // M4b: generalizes pickup_for_move()'s own "shrink the top undo group by
  // one" logic (main_window.cpp) to an ARBITRARY original index, not just
  // the tail. undo_group_sizes_ partitions document_.stamps() into
  // consecutive blocks counted back from the END (see that member's own
  // comment, below) -- `original_index` is a position in the CURRENT
  // (pre-removal) document_.stamps(), found by walking the groups back to
  // front until the one containing it is located, then shrinking that
  // group by exactly one (dropping it entirely if it was already down to
  // its last stamp). Must be called BEFORE document_.remove_stamp_at(),
  // since it reads document_.stamps().size() as the original list length
  // to derive each group's position. Deliberately does NOT touch
  // pickup_for_move() itself (M4a's Move mechanism) -- this is a new,
  // additive helper for Delete's own call site
  // (on_delete_edit_clicked()) only, even though the underlying reasoning
  // is the same generalization pickup_for_move()'s own comment already
  // walks through for its one fixed case (index == size()-1).
  void shrink_undo_group_containing(size_t original_index);

  SceneViewport *viewport_ = nullptr;
  QLabel *status_label_ = nullptr;

  QComboBox *shape_combo_ = nullptr;
  QComboBox *op_combo_ = nullptr;
  QDoubleSpinBox *size_spin_ = nullptr;
  QDoubleSpinBox *smoothness_spin_ = nullptr;
  // M3 (Looseness): 0 (clean/uniform, the default) to 1 (maximally loose).
  // Still purely authored/recorded, not wired to anything -- M6 (Flecks)
  // landed its own independent fleck_strength_spin_ below instead of
  // reading this: Looseness is stored PER-STAMP (SculptStamp::looseness,
  // CPU-side only, never uploaded to the GPU), while Fleck params are
  // PER-MATERIAL-SLOT (multiple stamps can share one slot) -- a real
  // granularity mismatch, not an oversight. Documented as a known gap
  // (same "shipped without X, noted why" precedent Spraypaint's missing
  // undo support already set), not blocking this milestone.
  QDoubleSpinBox *looseness_spin_ = nullptr;

  // M4a (Stretch): three independent multipliers (default 1.0, see
  // SceneViewport::kDefaultBrushStretch) layered on top of the Size
  // control above -- see SceneViewport::set_brush_stretch_x()/y()/z()'s
  // own comment for why three plain spin boxes rather than one combined
  // control.
  QDoubleSpinBox *stretch_x_spin_ = nullptr;
  QDoubleSpinBox *stretch_y_spin_ = nullptr;
  QDoubleSpinBox *stretch_z_spin_ = nullptr;

  // M5 (Finishes) panel widgets -- see build_brush_panel()'s own additions
  // and apply_finish_and_colour(). finish_combo_'s row index maps 1:1 onto
  // kFinishPresets (main_window.cpp) by position, same "enum/array order
  // is load-bearing" convention shape_combo_/kBrushShapeNames already use.
  QComboBox *finish_combo_ = nullptr;
  QPushButton *material_colour_button_ = nullptr;

  // M6 (Flecks) panel widgets -- see build_brush_panel()'s own additions
  // and apply_finish_and_colour(). fleck_combo_'s row index maps 1:1 onto
  // kFleckPresets (main_window.cpp) by position, and in turn onto Builtin.
  // FleckPattern.inc.glsl's 0-17 pattern id by the SAME position -- same
  // "enum/array order is load-bearing" convention shape_combo_/
  // kBrushShapeNames and finish_combo_/kFinishPresets already use.
  QComboBox *fleck_combo_ = nullptr;

  // M6 (Flecks): 0 (the startup default) means "no Fleck at all" --
  // whatever fleck_combo_ has selected is ignored engine-side the moment
  // this is 0 (see VulkanRaymarchShader::set_material()'s own fleck_
  // strength gating comment), which is exactly what makes a session that
  // never touches this slider allocate material slots identical to every
  // one M5 (Finishes) could already allocate -- the zero-regression
  // guarantee this milestone's own verification checks for.
  QDoubleSpinBox *fleck_strength_spin_ = nullptr;

  // M5 (Finishes): the base colour the NEXT allocated material slot gets
  // -- white matches ensure_default_sculpt_material()'s own slot-0
  // default, so a session that never opens the colour dialog behaves
  // identically to one where this milestone doesn't exist. Picked via
  // QColorDialog (on_pick_material_colour_clicked()).
  QColor current_material_colour_ = Qt::white;

  // M5 (Finishes): the next material slot apply_finish_and_colour() will
  // allocate, post-incremented on every call -- starts at 2, since slots 0
  // and 1 are reserved (VulkanRaymarchShader::ensure_default_sculpt_
  // material()'s plain-white default and Spraypaint's fixed paint
  // colour). Simplest-correct allocator for a hand-authored sculpting
  // session: every Finish/colour change gets its OWN fresh slot, never
  // reused, never freed -- kMaxScenePrimitives (1000, engine-side) is far
  // more headroom than one session's worth of deliberate material changes
  // will ever exhaust.
  i32 next_material_slot_ = 2;

  // M3b (Clone) panel widgets.
  QCheckBox *clone_mode_check_ = nullptr;
  QPushButton *set_clone_source_button_ = nullptr;
  QLabel *clone_source_label_ = nullptr;

  // M3c (Spraypaint) panel widgets -- mutually exclusive with clone_mode_
  // check_ above (see on_spray_mode_toggled()/on_clone_mode_toggled(), which
  // uncheck each other) -- Smear (the implicit default, both off) is the
  // third member of this exclusive set.
  QCheckBox *spray_mode_check_ = nullptr;
  QDoubleSpinBox *spray_opacity_spin_ = nullptr;

  // M4a (Move) panel widget -- mutually exclusive with clone_mode_check_/
  // spray_mode_check_ above (see on_move_mode_toggled()/on_clone_mode_
  // toggled()/on_spray_mode_toggled(), which all uncheck each other) --
  // Smear is the implicit default (all three off), same three-way-
  // exclusive-plus-default pattern Clone/Spraypaint already established.
  QCheckBox *move_mode_check_ = nullptr;

  // M4b (Delete-of-an-arbitrary-edit) panel widgets -- see
  // build_edit_history_panel()/refresh_edit_history_list().
  QListWidget *history_list_ = nullptr;
  QPushButton *delete_edit_button_ = nullptr;

  // M4c (Move-of-an-arbitrary-edit): a second button alongside delete_edit_
  // button_ in the same Edit History dock -- see build_edit_history_panel()/
  // on_move_edit_clicked()/on_edit_moved().
  QPushButton *move_edit_button_ = nullptr;

  // M4 (Crop/Cutout) panel widgets -- plain buttons in the brush panel
  // (reusing its existing shape/size/smoothness/stretch controls as the
  // crop shape source), not a dedicated panel -- see build_brush_panel()'s
  // own comment.
  QPushButton *crop_button_ = nullptr;
  QPushButton *cutout_button_ = nullptr;

  // This process's own record of every stamp applied so far -- see
  // sculpt_document.h's own comment for why this exists on the tool side
  // rather than reading it back from the engine's SculptVolume.
  SculptDocument document_;

  // M3a: redo history is now grouped -- one std::vector<SculptStampRecord>
  // per undone STROKE (a whole Smear drag, or a single plain click), not
  // one entry per stamp. redo_groups_.back() is the most recently undone
  // stroke; within a group, stamps are stored in the order they were
  // popped off document_ during the undo that created the group (i.e.
  // newest-of-the-group first, oldest-of-the-group last -- see
  // on_undo_triggered()), which on_redo_triggered() then replays back to
  // front (oldest first) to reconstruct the stroke in its original
  // stamping order. Still the tool-side mirror of VulkanRendererBackend's
  // own (flat, per-stamp) redo_stack_ -- grouping is purely a bookkeeping
  // layer on top of repeated single-stamp undo/redo calls, not a change to
  // what the engine itself tracks (see undo_group_sizes_'s own comment for
  // the full reasoning on why grouping lives here, tool-side, rather than
  // in the engine).
  std::vector<std::vector<SculptStampRecord>> redo_groups_;

  // M3a: one entry per completed stroke (a left-click, or a left-drag
  // Smear stroke), oldest first -- the size (in stamps) of each user-
  // visible undoable unit. Pushed by on_stroke_ended() when SceneViewport
  // reports a stroke finished, and by on_redo_triggered() when a redone
  // group becomes undoable again; popped by on_undo_triggered() to know
  // how many single-stamp renderer_sculpt_undo() calls make up "one
  // Ctrl+Z" for whatever stroke is on top.
  //
  // Reasoning for why this grouping concept lives in the TOOL rather than
  // the ENGINE (vulkan_raymarch_shader.h's undo_stack_/SculptSystem): the
  // engine already exposes exactly the primitive a grouped undo needs --
  // one undo_stamp()/stamp_shape() call per stamp, each independently
  // correct (restores/re-captures one SculptUndoEntry). Teaching the
  // engine a "stroke" concept (begin_stroke()/end_stroke(), undo_stamp()
  // gaining "how many consecutive-stroke stamps remain" awareness) would
  // duplicate bookkeeping the tool can do just as correctly by calling the
  // existing single-stamp API N times in a loop -- and the tool is already
  // the layer that knows when a drag starts/stops (SceneViewport's mouse
  // events), so it is also the natural layer to know how many stamps that
  // drag produced. This keeps vulkan_raymarch_shader.h/.cpp's undo_stack_
  // (and its kMaxSculptUndoDepth cap) completely untouched, at the cost of
  // Undo/Redo now being a *loop* over the existing per-stamp calls instead
  // of a single call -- a straightforward trade given M3a's scope (Smear
  // only, no new GPU work).
  std::vector<int> undo_group_sizes_;

  // Remembers the last path saved/loaded this session, purely so Ctrl+S
  // after a Load (or a prior Save) doesn't have to re-prompt -- still
  // falls back to a file dialog the first time either action runs.
  QString current_path_;
};
