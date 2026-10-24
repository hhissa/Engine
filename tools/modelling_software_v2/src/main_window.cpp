#include "main_window.h"
#include "scene_viewport.h"
#include "sculpt_shared.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace {
// Display names for SculptBrushShape's 15 values, IN THE SAME ORDER the
// enum declares them (renderer_types.inl) -- shape_combo_'s row index is
// cast straight to a SculptBrushShape (see ModellingWindow::
// on_shape_changed()), so this array's order is load-bearing, not just
// cosmetic.
constexpr const char *kBrushShapeNames[] = {
    "Sphere",         "Box",        "Plane",      "Torus",
    "Capped Cylinder", "Capped Cone", "Round Box",  "Box Frame",
    "Octahedron",     "Pyramid",    "Hex Prism",  "Round Cone",
    "Capsule",        "Link",       "Ellipsoid",
};
} // namespace

namespace {
// M5 (Finishes): the 10 Dreams-style material presets, each just a
// (roughness, metallic, waxy) triple -- see VulkanRaymarchShader::
// set_material()'s own header comment for exactly what each field does.
// finish_combo_'s row index is cast straight into this array (see
// ModellingWindow::apply_finish_and_colour()), so ORDER IS LOAD-BEARING,
// same convention kBrushShapeNames/shape_combo_ already use. Values match
// the plan file's own suggested starting point (joyful-skipping-giraffe.
// md §5) verbatim -- verified visually distinct from their neighbours in
// this milestone's own live screenshot pass (see its verification notes),
// so none needed adjusting.
struct FinishPreset {
  const char *name;
  f32 roughness;
  f32 metallic;
  bool waxy;
};
constexpr FinishPreset kFinishPresets[] = {
    {"Default", 0.6f, 0.0f, false},
    {"Plastic", 0.35f, 0.0f, false},
    {"Shiny", 0.08f, 0.0f, false},
    {"Rough", 0.9f, 0.0f, false},
    {"Metal", 0.35f, 1.0f, false},
    {"Wax", 0.6f, 0.0f, true},
    {"Shiny Metal", 0.08f, 1.0f, false},
    {"Shiny Wax", 0.08f, 0.0f, true},
    {"Rough Metal", 0.9f, 1.0f, false},
    {"Rough Wax", 0.9f, 0.0f, true},
    // Clay: soft, hand-worked damp clay -- matte-leaning (rougher than Wax's
    // 0.6, so it doesn't glow) but not as flat/chalky as Rough's 0.9. Waxy
    // is on for the same reason Wax's own second specular lobe exists: damp
    // clay still catches a faint, soft sheen at grazing angles, just far
    // more muted than Wax's at 0.6 roughness -- the waxy lobe's own
    // roughness-dependent falloff does the "subtler, not glossier" work
    // here, not a separate parameter. Appended at the end -- see this
    // array's own "ORDER IS LOAD-BEARING" header comment above.
    {"Clay", 0.78f, 0.0f, true},
};
} // namespace

namespace {
// Stops SceneViewport's render timer for as long as this guard is alive --
// construct one at the top of any handler that shows a modal dialog
// (QFileDialog spins its own nested Qt event loop) and let it go out of
// scope when the handler returns. See SceneViewport::pause_rendering()'s
// own comment for why leaving the render timer running through a modal
// dialog would actually freeze the whole application, not just the 3D
// view. Carried over from tools/sdf_editor's identical ScopedRenderPause
// (git history).
class ScopedRenderPause {
public:
  explicit ScopedRenderPause(SceneViewport *viewport) : viewport_(viewport) {
    viewport_->pause_rendering();
  }
  ~ScopedRenderPause() { viewport_->resume_rendering(); }

  ScopedRenderPause(const ScopedRenderPause &) = delete;
  ScopedRenderPause &operator=(const ScopedRenderPause &) = delete;

private:
  SceneViewport *viewport_;
};
} // namespace

ModellingWindow::ModellingWindow() {
  setWindowTitle("Modelling Software v2");
  resize(1280, 800);

  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);

  // The live rendered scene (see SceneViewport) -- right-drag orbits,
  // wheel zooms, WASD+QE flies, left-click stamps whatever the brush
  // panel (see build_brush_panel() below) currently has selected, at a
  // fixed distance along the click ray.
  viewport_ = new SceneViewport();
  connect(viewport_, &SceneViewport::stamp_placed, this,
          &ModellingWindow::on_stamp_placed);
  // M3a: reports how many stamps the just-finished left-drag (or plain
  // click) produced, so it can be undone/redone as one unit -- see
  // on_stroke_ended()/undo_group_sizes_'s own comment.
  connect(viewport_, &SceneViewport::stroke_ended, this,
          &ModellingWindow::on_stroke_ended);
  // M3b (Clone): tells the user why a Clone-mode drag just did nothing
  // (no source captured yet) -- see SceneViewport::clone_source_missing()'s
  // own comment.
  connect(viewport_, &SceneViewport::clone_source_missing, this,
          &ModellingWindow::on_clone_source_missing);
  // M4a (Move): tells the user why a Move-mode drag just did nothing
  // (nothing stamped yet this session) -- mirrors the Clone wiring above
  // exactly, see SceneViewport::move_source_missing()'s own comment.
  connect(viewport_, &SceneViewport::move_source_missing, this,
          &ModellingWindow::on_move_source_missing);
  // M4a (Move): installs this window's pickup_for_move() as viewport_'s
  // MovePickupFn -- see SceneViewport::set_move_pickup_callback()'s own
  // comment for why this is a std::function rather than a Qt signal
  // (pickup needs a synchronous return value, document_ lives here, not
  // on SceneViewport). A plain lambda rather than a direct member-function
  // pointer bind, since pickup_for_move() is private.
  viewport_->set_move_pickup_callback(
      [this]() { return pickup_for_move(); });
  // M4c (Move-of-an-arbitrary-edit): fires once a begin_move_selected_edit()
  // -armed drag actually commits a new position -- see SceneViewport::
  // edit_moved()'s own comment and on_edit_moved() below.
  connect(viewport_, &SceneViewport::edit_moved, this,
          &ModellingWindow::on_edit_moved);
  QWidget *viewport_container =
      QWidget::createWindowContainer(viewport_, central);
  // Keeps the swapchain from ever seeing a 0x0 extent (e.g. if the window
  // starts very small).
  viewport_container->setMinimumSize(320, 240);
  layout->addWidget(viewport_container);

  setCentralWidget(central);

  // --- File menu: Save/Load the tool-side stamp list (see
  // sculpt_document.h). Ctrl+S/Ctrl+O match the usual save/open
  // convention; also exposed as plain menu actions so the shortcut isn't
  // the only way to reach them.
  QMenu *file_menu = menuBar()->addMenu("&File");
  QAction *save_action = file_menu->addAction("&Save Stamps...");
  save_action->setShortcut(QKeySequence::Save);
  connect(save_action, &QAction::triggered, this,
          &ModellingWindow::on_save_triggered);

  QAction *load_action = file_menu->addAction("&Load Stamps...");
  load_action->setShortcut(QKeySequence::Open);
  connect(load_action, &QAction::triggered, this,
          &ModellingWindow::on_load_triggered);

  // M4 (Cutout): opens a standalone snapshot file a PRIOR Cutout saved
  // (see on_cutout_clicked()) -- deliberately a separate menu action from
  // "Load Stamps..." above, not folded into it: a cutout file is NOT the
  // tool's .stamps replayable-edit-list format (see SceneViewport::
  // load_cutout_file()'s own comment), so it needs its own loader path,
  // not a new branch inside SculptDocument::load(). Mirrors the plan's own
  // "open the removed portion as a separate session of the tool" --
  // loading it into THIS session (additive, same caveat the ordinary Load
  // path already has) is equally supported; opening it in a freshly
  // launched, nothing-stamped-yet session is the verified, intended use.
  QAction *open_cutout_action = file_menu->addAction("Open Cutout...");
  connect(open_cutout_action, &QAction::triggered, this,
          &ModellingWindow::on_open_cutout_triggered);

  // --- Edit menu: Undo/Redo the most recent stamp (M2b). QKeySequence::
  // Undo/Redo resolve to the platform's usual Ctrl+Z/Ctrl+Shift+Z (X11/
  // most Linux desktops) -- same "match the standard sequence, also expose
  // a plain menu action" pattern as Save/Load above.
  QMenu *edit_menu = menuBar()->addMenu("&Edit");
  QAction *undo_action = edit_menu->addAction("&Undo Stamp");
  undo_action->setShortcut(QKeySequence::Undo);
  connect(undo_action, &QAction::triggered, this,
          &ModellingWindow::on_undo_triggered);

  QAction *redo_action = edit_menu->addAction("&Redo Stamp");
  redo_action->setShortcut(QKeySequence::Redo);
  connect(redo_action, &QAction::triggered, this,
          &ModellingWindow::on_redo_triggered);

  build_brush_panel();
  build_edit_history_panel();

  status_label_ = new QLabel(this);
  statusBar()->addWidget(status_label_);
  update_status_label();
}

void ModellingWindow::build_brush_panel() {
  // M2a: a minimal dock panel (shape dropdown, op toggle, size/smoothness
  // spin boxes) over SceneViewport's own brush state -- deliberately not a
  // polished authoring UI (see sculpt_document.h's own comment on the
  // size-to-params mapping this panel's one "Size" control drives); it
  // only needs to prove shape/smoothness/subtraction work end to end.
  auto *dock = new QDockWidget("Brush", this);
  dock->setFeatures(QDockWidget::DockWidgetMovable |
                    QDockWidget::DockWidgetFloatable);
  auto *panel = new QWidget(dock);
  auto *form = new QFormLayout(panel);

  shape_combo_ = new QComboBox(panel);
  for (const char *name : kBrushShapeNames) {
    shape_combo_->addItem(name);
  }
  shape_combo_->setCurrentIndex(0); // Sphere, matching SceneViewport's default.
  connect(shape_combo_, &QComboBox::currentIndexChanged, this,
          &ModellingWindow::on_shape_changed);
  form->addRow("Shape", shape_combo_);

  op_combo_ = new QComboBox(panel);
  op_combo_->addItem("Union");
  op_combo_->addItem("Subtract");
  op_combo_->setCurrentIndex(0);
  connect(op_combo_, &QComboBox::currentIndexChanged, this,
          &ModellingWindow::on_op_changed);
  form->addRow("Op", op_combo_);

  size_spin_ = new QDoubleSpinBox(panel);
  size_spin_->setRange(0.05, 20.0);
  size_spin_->setSingleStep(0.1);
  size_spin_->setValue(SceneViewport::kDefaultBrushSize);
  connect(size_spin_, &QDoubleSpinBox::valueChanged, this,
          &ModellingWindow::on_size_changed);
  form->addRow("Size", size_spin_);

  smoothness_spin_ = new QDoubleSpinBox(panel);
  smoothness_spin_->setRange(0.0, 5.0);
  smoothness_spin_->setSingleStep(0.05);
  smoothness_spin_->setValue(0.0); // 0 = hard seam, matching SceneViewport's default.
  connect(smoothness_spin_, &QDoubleSpinBox::valueChanged, this,
          &ModellingWindow::on_smoothness_changed);
  form->addRow("Smoothness", smoothness_spin_);

  // M3 (Looseness): 0..1, no visible effect yet (see looseness_spin_'s own
  // comment, main_window.h) -- included so an authored value survives
  // save/load/Undo/Redo.
  looseness_spin_ = new QDoubleSpinBox(panel);
  looseness_spin_->setRange(0.0, 1.0);
  looseness_spin_->setSingleStep(0.05);
  looseness_spin_->setValue(0.0); // 0 = clean/uniform, matching SceneViewport's default.
  looseness_spin_->setToolTip(
      "No visible effect yet. Authored values are saved/undoable.");
  connect(looseness_spin_, &QDoubleSpinBox::valueChanged, this,
          &ModellingWindow::on_looseness_changed);
  form->addRow("Looseness", looseness_spin_);

  // M4a (Stretch): three independent multipliers, default 1.0 (= no
  // stretch, matching SceneViewport::kDefaultBrushStretch) -- see
  // SceneViewport::set_brush_stretch_x()/y()/z()'s own comment. Range/step
  // mirror Size's own spin box above (0.1 floor rather than Size's 0.05,
  // since a near-zero stretch on one axis degenerates a shape to a sliver
  // -- still a valid, if uninteresting, SDF, no need to forbid it further
  // than Size itself already allows for degenerate inputs).
  stretch_x_spin_ = new QDoubleSpinBox(panel);
  stretch_x_spin_->setRange(0.1, 10.0);
  stretch_x_spin_->setSingleStep(0.1);
  stretch_x_spin_->setValue(SceneViewport::kDefaultBrushStretch);
  connect(stretch_x_spin_, &QDoubleSpinBox::valueChanged, this,
          &ModellingWindow::on_stretch_x_changed);
  form->addRow("Stretch X", stretch_x_spin_);

  stretch_y_spin_ = new QDoubleSpinBox(panel);
  stretch_y_spin_->setRange(0.1, 10.0);
  stretch_y_spin_->setSingleStep(0.1);
  stretch_y_spin_->setValue(SceneViewport::kDefaultBrushStretch);
  connect(stretch_y_spin_, &QDoubleSpinBox::valueChanged, this,
          &ModellingWindow::on_stretch_y_changed);
  form->addRow("Stretch Y", stretch_y_spin_);

  stretch_z_spin_ = new QDoubleSpinBox(panel);
  stretch_z_spin_->setRange(0.1, 10.0);
  stretch_z_spin_->setSingleStep(0.1);
  stretch_z_spin_->setValue(SceneViewport::kDefaultBrushStretch);
  connect(stretch_z_spin_, &QDoubleSpinBox::valueChanged, this,
          &ModellingWindow::on_stretch_z_changed);
  form->addRow("Stretch Z", stretch_z_spin_);

  // M5 (Finishes): a Finish preset dropdown + a base-colour picker, added
  // to the same dock/form as every other brush-panel control -- "what
  // will the NEXT stamp look like" is exactly the same kind of control as
  // Shape/Size/Smoothness above, just one more dimension of it (see the
  // task's own "keep it simple" scope: no materials-list UI, no per-slot
  // editing after the fact). Neither control touches the engine directly
  // by itself -- see apply_finish_and_colour() for why BOTH funnel through
  // one shared function that allocates a fresh slot and configures it.
  finish_combo_ = new QComboBox(panel);
  for (const FinishPreset &preset : kFinishPresets) {
    finish_combo_->addItem(preset.name);
  }
  finish_combo_->setCurrentIndex(0); // Default, matching slot 0's own look.
  connect(finish_combo_, &QComboBox::currentIndexChanged, this,
          &ModellingWindow::on_finish_changed);
  form->addRow("Finish", finish_combo_);

  material_colour_button_ = new QPushButton("Material Colour...", panel);
  connect(material_colour_button_, &QPushButton::clicked, this,
          &ModellingWindow::on_pick_material_colour_clicked);
  form->addRow(material_colour_button_);

  // M3b: Clone controls, added to the same dock/form rather than a separate
  // panel -- Clone is just another drag mode layered on top of the same
  // brush shape/size/smoothness/op controls above (whatever those are set
  // to when "Set Clone Source" is clicked IS the clone source), so keeping
  // them visually grouped together matches how they actually relate.
  // "Enable Clone Mode" is a checkbox (not a second radio-style button, see
  // the task's own "your call" on placement) because Clone/Smear read as a
  // toggle layered on the existing brush panel rather than a third
  // independent tool selector -- a drag either re-reads the live panel
  // (Smear, checkbox off) or re-stamps the frozen clipboard (Clone,
  // checkbox on); nothing else about the panel changes meaning either way.
  clone_mode_check_ = new QCheckBox("Enable Clone Mode", panel);
  connect(clone_mode_check_, &QCheckBox::toggled, this,
          &ModellingWindow::on_clone_mode_toggled);
  form->addRow(clone_mode_check_);

  set_clone_source_button_ = new QPushButton("Set Clone Source", panel);
  connect(set_clone_source_button_, &QPushButton::clicked, this,
          &ModellingWindow::on_set_clone_source_clicked);
  form->addRow(set_clone_source_button_);

  clone_source_label_ = new QLabel(panel);
  form->addRow(clone_source_label_);
  update_clone_source_label();

  // M3c: Spraypaint controls -- a THIRD, mutually-exclusive drag mode
  // alongside Smear (default, both this and Clone off) and Clone above
  // (see on_spray_mode_toggled()/on_clone_mode_toggled(), which uncheck
  // each other). Reuses the same brush shape/size controls above (a spray
  // pass's extent comes from whatever shape/size the panel already has
  // selected -- see SceneViewport::place_spray()); "Spray Opacity" is the
  // one new control this mode needs, controlling how much a single pass
  // builds up (see SceneViewport::set_spray_opacity_per_pass()'s own
  // comment).
  spray_mode_check_ = new QCheckBox("Enable Spraypaint Mode", panel);
  connect(spray_mode_check_, &QCheckBox::toggled, this,
          &ModellingWindow::on_spray_mode_toggled);
  form->addRow(spray_mode_check_);

  spray_opacity_spin_ = new QDoubleSpinBox(panel);
  spray_opacity_spin_->setRange(0.01, 1.0);
  spray_opacity_spin_->setSingleStep(0.05);
  spray_opacity_spin_->setValue(SceneViewport::kDefaultSprayOpacityPerPass);
  connect(spray_opacity_spin_, &QDoubleSpinBox::valueChanged, this,
          &ModellingWindow::on_spray_opacity_changed);
  form->addRow("Spray Opacity", spray_opacity_spin_);

  // M4a (Move): a FOURTH drag mode, mutually exclusive with Clone/
  // Spraypaint above (Smear is the implicit default, all three off) -- see
  // on_move_mode_toggled()'s own comment for the uncheck-the-others wiring.
  // No extra controls beyond the toggle itself: Move re-stamps the picked-
  // up shape's own ORIGINAL recorded configuration, not anything the brush
  // panel currently holds, so there is nothing else here for it to read.
  move_mode_check_ = new QCheckBox("Enable Move Mode", panel);
  connect(move_mode_check_, &QCheckBox::toggled, this,
          &ModellingWindow::on_move_mode_toggled);
  form->addRow(move_mode_check_);

  // M4 (Crop/Cutout): two plain buttons, added to the SAME dock/form as
  // every brush-shape control above -- "the current brush configuration
  // is a reasonable crop shape source" (mirroring how Clone's "Set Clone
  // Source" above captures "whatever the brush panel currently holds").
  // Crop/Cutout always target the viewport's own center-ray point (see
  // SceneViewport::do_crop()'s own comment) -- no separate position
  // control exists, same "simple buttons, no dedicated panel" scope the
  // task explicitly allows.
  crop_button_ = new QPushButton("Crop (keep inside brush shape)", panel);
  connect(crop_button_, &QPushButton::clicked, this,
          &ModellingWindow::on_crop_clicked);
  form->addRow(crop_button_);

  cutout_button_ = new QPushButton("Cutout... (crop + save removed part)",
                                   panel);
  connect(cutout_button_, &QPushButton::clicked, this,
          &ModellingWindow::on_cutout_clicked);
  form->addRow(cutout_button_);

  dock->setWidget(panel);
  addDockWidget(Qt::RightDockWidgetArea, dock);
}

void ModellingWindow::build_edit_history_panel() {
  // M4b: a minimal dock (plain list + one button) over document_.stamps()
  // -- deliberately not a polished authoring UI, same "prove the mechanism
  // works end to end" scope build_brush_panel()'s own comment already
  // claims for the brush panel. Bottom dock area: a list reads more
  // naturally wide-and-short than squeezed into the brush panel's existing
  // narrow right-hand column.
  auto *dock = new QDockWidget("Edit History", this);
  dock->setFeatures(QDockWidget::DockWidgetMovable |
                    QDockWidget::DockWidgetFloatable);
  auto *panel = new QWidget(dock);
  auto *layout = new QVBoxLayout(panel);

  history_list_ = new QListWidget(panel);
  layout->addWidget(history_list_);

  delete_edit_button_ = new QPushButton("Delete Selected Edit", panel);
  connect(delete_edit_button_, &QPushButton::clicked, this,
          &ModellingWindow::on_delete_edit_clicked);
  layout->addWidget(delete_edit_button_);

  // M4c (Move-of-an-arbitrary-edit): a second button alongside Delete --
  // arms SceneViewport's pending_move_edit_index_ for the selected row (see
  // on_move_edit_clicked()); the actual move commits on the NEXT drag's
  // release in the viewport, not here (there is no gizmo in this tool, so
  // "drag in the viewport to pick a new position" is the simplest correct
  // interaction -- see the task's own UI note).
  move_edit_button_ = new QPushButton("Move Selected Edit", panel);
  connect(move_edit_button_, &QPushButton::clicked, this,
          &ModellingWindow::on_move_edit_clicked);
  layout->addWidget(move_edit_button_);

  dock->setWidget(panel);
  addDockWidget(Qt::BottomDockWidgetArea, dock);
}

void ModellingWindow::on_shape_changed(int index) {
  if (index < 0 ||
      index >= static_cast<int>(sizeof(kBrushShapeNames) /
                                sizeof(kBrushShapeNames[0]))) {
    return;
  }
  viewport_->set_brush_shape(static_cast<SculptBrushShape>(index));
}

void ModellingWindow::on_op_changed(int index) {
  viewport_->set_brush_op(index == 1 ? SculptBrushOp::Subtraction
                                     : SculptBrushOp::Union);
}

void ModellingWindow::on_smoothness_changed(double value) {
  viewport_->set_brush_smoothness(static_cast<f32>(value));
}

void ModellingWindow::on_looseness_changed(double value) {
  viewport_->set_brush_looseness(static_cast<f32>(value));
}

void ModellingWindow::on_size_changed(double value) {
  viewport_->set_brush_size(static_cast<f32>(value));
}

void ModellingWindow::on_clone_mode_toggled(bool checked) {
  viewport_->set_clone_mode(checked);
  // M3c/M4a: Clone/Spraypaint/Move are mutually exclusive -- turning Clone
  // on turns the other two back off. setChecked(false) on an unchecked box
  // is a no-op (no spurious toggled(false) signal), and on a checked box
  // it fires that box's own toggled(false) handler, which only touches its
  // own viewport_ state -- no risk of bouncing back here.
  if (checked && spray_mode_check_ && spray_mode_check_->isChecked()) {
    spray_mode_check_->setChecked(false);
  }
  if (checked && move_mode_check_ && move_mode_check_->isChecked()) {
    move_mode_check_->setChecked(false);
  }
}

void ModellingWindow::on_set_clone_source_clicked() {
  // Captures whatever the brush panel (shape/op/smoothness/size combo
  // boxes above) currently holds -- independent of whether "Enable Clone
  // Mode" is checked, so a source can be pre-staged before turning Clone
  // on, or swapped out for a different one between drags while Clone mode
  // stays on.
  viewport_->capture_clone_source();
  update_clone_source_label();
}

void ModellingWindow::on_spray_mode_toggled(bool checked) {
  viewport_->set_spray_mode(checked);
  // M3c/M4a: same mutual-exclusion reasoning as on_clone_mode_toggled()
  // above, the other two directions.
  if (checked && clone_mode_check_ && clone_mode_check_->isChecked()) {
    clone_mode_check_->setChecked(false);
  }
  if (checked && move_mode_check_ && move_mode_check_->isChecked()) {
    move_mode_check_->setChecked(false);
  }
}

void ModellingWindow::on_spray_opacity_changed(double value) {
  viewport_->set_spray_opacity_per_pass(static_cast<f32>(value));
}

void ModellingWindow::on_move_mode_toggled(bool checked) {
  viewport_->set_move_mode(checked);
  // M4a: same mutual-exclusion reasoning as on_clone_mode_toggled()/on_
  // spray_mode_toggled() above, the remaining two directions.
  if (checked && clone_mode_check_ && clone_mode_check_->isChecked()) {
    clone_mode_check_->setChecked(false);
  }
  if (checked && spray_mode_check_ && spray_mode_check_->isChecked()) {
    spray_mode_check_->setChecked(false);
  }
}

void ModellingWindow::on_stretch_x_changed(double value) {
  viewport_->set_brush_stretch_x(static_cast<f32>(value));
}

void ModellingWindow::on_stretch_y_changed(double value) {
  viewport_->set_brush_stretch_y(static_cast<f32>(value));
}

void ModellingWindow::on_stretch_z_changed(double value) {
  viewport_->set_brush_stretch_z(static_cast<f32>(value));
}

void ModellingWindow::on_finish_changed(int index) {
  if (index < 0 ||
      index >= static_cast<int>(sizeof(kFinishPresets) /
                                sizeof(kFinishPresets[0]))) {
    return;
  }
  apply_finish_and_colour();
}

void ModellingWindow::on_pick_material_colour_clicked() {
  // QColorDialog spins its own nested Qt event loop, exactly like
  // QFileDialog -- see ScopedRenderPause's own comment for why the
  // viewport's render timer must be paused around it.
  ScopedRenderPause pause(viewport_);
  QColor picked = QColorDialog::getColor(current_material_colour_, this,
                                         "Material Colour");
  if (!picked.isValid()) {
    return; // Dialog was cancelled -- leave the current colour untouched.
  }
  current_material_colour_ = picked;
  apply_finish_and_colour();
}

void ModellingWindow::apply_finish_and_colour() {
  if (!viewport_ || !finish_combo_) {
    return;
  }
  const int index = finish_combo_->currentIndex();
  if (index < 0 ||
      index >= static_cast<int>(sizeof(kFinishPresets) /
                                sizeof(kFinishPresets[0]))) {
    return;
  }
  const FinishPreset &preset = kFinishPresets[index];
  const glm::vec3 colour(static_cast<f32>(current_material_colour_.redF()),
                        static_cast<f32>(current_material_colour_.greenF()),
                        static_cast<f32>(current_material_colour_.blueF()));

  // M5 (Finishes): a FRESH slot every time, never reused --
  // see next_material_slot_'s own comment (main_window.h) for why that is
  // the simplest correct allocator for this tool, and why NOT
  // retroactively changing already-stamped geometry (this only affects
  // what viewport_->set_brush_material_slot() points subsequent stamps
  // at) is consistent with every other brush-panel control.
  const i32 slot = next_material_slot_++;
  viewport_->configure_material(slot, colour, preset.roughness,
                                preset.metallic, preset.waxy);
  viewport_->set_brush_material_slot(slot);

  // Mirrors the colour back onto the button itself, so the panel shows
  // what's actually about to be stamped rather than just the preset name.
  material_colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(current_material_colour_.name()));

  statusBar()->showMessage(
      QString("Finish '%1' + colour %2 -> material slot %3 (next stamp).")
          .arg(QString(preset.name), current_material_colour_.name())
          .arg(slot),
      4000);
}

void ModellingWindow::on_clone_source_missing() {
  statusBar()->showMessage(
      "Clone mode is on but no source has been captured yet -- click "
      "'Set Clone Source' first.",
      4000);
}

void ModellingWindow::on_move_source_missing() {
  statusBar()->showMessage(
      "Move mode is on but nothing has been stamped yet -- nothing to "
      "move.",
      4000);
}

void ModellingWindow::update_clone_source_label() {
  if (!viewport_->has_clone_source()) {
    clone_source_label_->setText("Clone Source: none captured");
    return;
  }
  int shape_index = static_cast<int>(viewport_->clone_source_shape());
  const char *shape_name =
      (shape_index >= 0 &&
       shape_index < static_cast<int>(sizeof(kBrushShapeNames) /
                                      sizeof(kBrushShapeNames[0])))
          ? kBrushShapeNames[shape_index]
          : "?";
  const char *op_name = viewport_->clone_source_op() == SculptBrushOp::Subtraction
                            ? "Subtract"
                            : "Union";
  clone_source_label_->setText(
      QString("Clone Source: %1, size %2, %3")
          .arg(shape_name)
          .arg(viewport_->clone_source_size(), 0, 'f', 2)
          .arg(op_name));
}

ModellingWindow::~ModellingWindow() {
  // Shuts the renderer down explicitly while the viewport's native window
  // handle is still alive -- relying on SceneViewport's own destructor
  // running first (via QObject child-deletion order) isn't guaranteed to
  // happen before Qt starts tearing down the native window itself.
  // Mirrors tools/sdf_editor's identical shutdown-ordering note (git
  // history).
  if (viewport_) {
    viewport_->shutdown_renderer();
  }
}

void ModellingWindow::on_stamp_placed(glm::vec3 center,
                                       SculptBrushShape shape_type,
                                       glm::vec4 params, f32 smoothness,
                                       SculptBrushOp op, int material_slot,
                                       f32 looseness) {
  // The object is whichever one the engine just recorded the stamp under (a
  // new one per stroke -- see SceneViewport::mousePressEvent()).
  document_.add_stamp(center, shape_type, params, smoothness, op,
                      material_slot, looseness,
                      viewport_->last_stamp_rotation(),
                      renderer_sculpt_current_object());
  // A fresh, user-driven stamp invalidates any pending redo history -- the
  // engine side (VulkanRendererBackend::stamp_shape()) clears its own redo_
  // stack_ for exactly the same reason at exactly the same moment (this
  // slot runs off SceneViewport::stamp_placed(), emitted right after the
  // call that triggers that engine-side clear -- see scene_viewport.cpp's
  // place_stamp()), so the two stay in lockstep. M3a: this fires once per
  // stamp during a Smear drag too (not just once per click), so mid-stroke
  // this clears redo_groups_ repeatedly -- harmless, since a drag that
  // produces a fresh stamp is exactly the kind of new edit that should
  // invalidate any old forward history, same as a single click always did.
  // Does NOT touch undo_group_sizes_: that stack records already-COMPLETED
  // strokes, which a still-in-progress stroke's sub-stamps don't affect
  // (see on_stroke_ended(), which appends this stroke's own entry only
  // once it finishes).
  redo_groups_.clear();
  update_status_label();
}

void ModellingWindow::on_stroke_ended(int stamp_count) {
  // M3a: finalizes the stroke that just finished (a left-click reports 1;
  // a Smear drag reports however many sub-stamps actually fired) as one
  // undoable unit -- see undo_group_sizes_'s own comment for why this
  // grouping lives here rather than in the engine. stamp_count <= 0 would
  // mean SceneViewport reported a stroke that placed nothing (shouldn't
  // happen -- mousePressEvent always stamps at least once before
  // left_dragging_ is set -- but guarded defensively rather than pushing a
  // vacuous group).
  if (stamp_count > 0) {
    undo_group_sizes_.push_back(stamp_count);
  }
}

void ModellingWindow::on_undo_triggered() {
  // M3a: one Ctrl+Z undoes a whole STROKE, not one stamp -- pop however
  // many stamps the most recently completed stroke placed (falling back
  // to 1 if undo_group_sizes_ is empty, which should only happen for
  // stamps that predate this grouping, e.g. none in practice since this
  // lands before any released build, but kept as a safe default rather
  // than silently doing nothing) and call the engine's existing single-
  // stamp undo_last_stamp() that many times in a loop.
  int remaining = 1;
  if (!undo_group_sizes_.empty()) {
    remaining = undo_group_sizes_.back();
    undo_group_sizes_.pop_back();
  }

  // Collected in pop order (most-recent-of-the-stroke first, oldest-of-
  // the-stroke last) -- matches document_.pop_last_stamp()'s own LIFO
  // order, and mirrors exactly how the engine's own redo_stack_ fills up
  // across these same undo_last_stamp() calls (see VulkanRendererBackend::
  // undo_stamp()'s push_back(popped)), which is why on_redo_triggered()
  // below can just pop_back() this same vector to replay oldest-first.
  std::vector<SculptStampRecord> group;
  for (int i = 0; i < remaining; ++i) {
    if (!viewport_->undo_last_stamp()) {
      // Engine ran out of undo history partway through the stroke (e.g.
      // kMaxSculptUndoDepth was exceeded earlier this session) -- stop;
      // whatever was undone so far is already reflected in document_, and
      // is still captured as a (smaller) redo group below.
      break;
    }
    if (std::optional<SculptStampRecord> popped = document_.pop_last_stamp()) {
      group.push_back(*popped);
    }
  }
  if (!group.empty()) {
    redo_groups_.push_back(std::move(group));
  }
  update_status_label();
}

void ModellingWindow::on_redo_triggered() {
  if (redo_groups_.empty()) {
    return;
  }
  std::vector<SculptStampRecord> group = std::move(redo_groups_.back());
  redo_groups_.pop_back();

  // group.back() is the OLDEST stamp of the stroke (see on_undo_triggered()
  // 's own comment on fill order) -- popping from the back here replays
  // the stroke oldest-stamp-first, reconstructing it in its original
  // stamping order, and matches the engine's own redo_stack_ pop order
  // exactly (VulkanRendererBackend::redo_stamp() also pops its back()),
  // so viewport_->redo_last_stamp() (which carries no parameters of its
  // own -- it replays whatever the engine's redo_stack_ has on top) always
  // lines up with the record we re-add to document_ here.
  int redone_count = 0;
  while (!group.empty()) {
    if (!viewport_->redo_last_stamp()) {
      break;
    }
    const SculptStampRecord stamp = group.back();
    group.pop_back();
    document_.add_stamp(stamp.center, stamp.shape_type, stamp.params,
                        stamp.smoothness, stamp.op, stamp.material_slot,
                        stamp.looseness, stamp.rotation, stamp.object_id);
    ++redone_count;
  }
  if (!group.empty()) {
    // Partial redo (engine-side redo_stack_ ran dry mid-group, which
    // shouldn't happen since it was filled by the matching undo, but
    // guarded the same way on_undo_triggered() guards its own loop) --
    // push whatever's left back as its own smaller group rather than
    // losing it.
    redo_groups_.push_back(std::move(group));
  }
  // The stroke we just replayed is, once again, one undoable unit.
  if (redone_count > 0) {
    undo_group_sizes_.push_back(redone_count);
  }
  update_status_label();
}

std::optional<SculptStampRecord> ModellingWindow::pickup_for_move() {
  // M4a (Move): "the most recently placed stamp" is resolved against
  // document_ -- the tool-side mirror kept in lockstep with the engine's
  // own undo-stack top by every prior milestone's Undo/Redo handler above
  // (see on_undo_triggered()/on_redo_triggered()), so document_.stamps().
  // back() and the engine's top-of-undo_stack_ always agree. Reading
  // document_ here (rather than adding a new engine-side "peek the top
  // stamp's params" API) is deliberate -- the task this landed under
  // explicitly calls out that Move needs no new engine capability at all,
  // and document_ already has exactly what's needed.
  if (document_.empty()) {
    return std::nullopt;
  }
  // GPU/engine-side first, same "never claim more than actually happened"
  // discipline on_undo_triggered() follows -- only retract document_'s
  // record if the engine confirms its own matching stamp was actually
  // reverted.
  if (!viewport_->undo_last_stamp()) {
    return std::nullopt;
  }
  std::optional<SculptStampRecord> popped = document_.pop_last_stamp();
  if (!popped.has_value()) {
    // Shouldn't happen (document_ wasn't empty a moment ago, and the
    // engine just confirmed an undo happened) -- but if the two somehow
    // disagree, there is nothing coherent to hand back to Move.
    return std::nullopt;
  }

  // Bookkeeping: a pickup IS mechanically an Undo of exactly one stamp
  // (see SceneViewport::mousePressEvent()'s Move branch) -- but unlike
  // on_undo_triggered(), it never undoes a whole STROKE, only the single
  // topmost stamp, regardless of which stroke-group that stamp happened
  // to belong to. So rather than popping undo_group_sizes_.back() whole
  // (which would be wrong whenever the picked-up stamp was only PART of a
  // multi-stamp Smear/Clone stroke, leaving the group's remaining stamps
  // with no undo-grouping entry of their own), shrink the top group by
  // exactly one -- or drop it entirely if it was already down to its last
  // stamp (the common case: a single click). This keeps a later Ctrl+Z on
  // whatever's left of that stroke calling the engine's undo_last_stamp()
  // exactly as many times as stamps actually remain in it.
  if (!undo_group_sizes_.empty()) {
    if (undo_group_sizes_.back() <= 1) {
      undo_group_sizes_.pop_back();
    } else {
      --undo_group_sizes_.back();
    }
  }

  // Mirrors on_undo_triggered()'s own redo_groups_.push_back() -- a pickup
  // is mechanically an Undo (see above), so it builds forward history the
  // same way, as a safety net in case this gesture is somehow abandoned
  // before SceneViewport::place_move_stamp() ever commits a new stamp
  // (e.g. the drag loses its mouse grab) -- Ctrl+Shift+Z would still
  // restore the original position in that case. In the ordinary case (the
  // drag completes normally), on_stamp_placed() unconditionally clears
  // redo_groups_ a moment later when the move commits -- discarding this
  // entry along with everything else, which is exactly the chosen "Undo
  // after a COMPLETED Move removes the shape entirely, rather than
  // restoring the original position" semantics: both are defensible: see
  // SceneViewport::mouseReleaseEvent()'s own comment for the engine-side
  // mechanics (stamp_shape()'s redo_stack_.clear()) that make "removes
  // entirely" the natural, no-special-casing outcome of implementing Move
  // as "undo + re-stamp" rather than "undo + restore-on-undo".
  redo_groups_.push_back({*popped});

  update_status_label();
  return popped;
}

void ModellingWindow::update_status_label() {
  status_label_->setText(QString("Stamps: %1%2")
                              .arg(document_.stamps().size())
                              .arg(current_path_.isEmpty()
                                       ? QString()
                                       : QString(" -- %1").arg(current_path_)));
  // M4b: refreshed from the exact same call site update_status_label()
  // already has -- a fresh stamp, Undo, Redo, Load, a Move pickup, and now
  // Delete all funnel through here, so the history list never needs its
  // own separate set of hook points.
  refresh_edit_history_list();
}

void ModellingWindow::refresh_edit_history_list() {
  if (!history_list_) {
    return; // Called once from the constructor's own update_status_label()
           // before build_edit_history_panel() has run yet -- no-op.
  }
  // Preserve the current row across a refresh that doesn't shrink the list
  // out from under it (e.g. an Undo that popped a stroke other than
  // whatever row is currently selected) -- otherwise every single stamp
  // placed anywhere would silently clear whatever the user had selected
  // for a future Delete.
  const int previous_row = history_list_->currentRow();
  history_list_->clear();
  const std::vector<SculptStampRecord> &stamps = document_.stamps();
  for (size_t i = 0; i < stamps.size(); ++i) {
    const SculptStampRecord &stamp = stamps[i];
    const int shape_index = static_cast<int>(stamp.shape_type);
    const char *shape_name =
        (shape_index >= 0 &&
         shape_index < static_cast<int>(sizeof(kBrushShapeNames) /
                                        sizeof(kBrushShapeNames[0])))
            ? kBrushShapeNames[shape_index]
            : "?";
    // M7 (edit-history UI polish): added the (Sub) suffix on top of the
    // shape/position info this list already had -- Union is overwhelmingly
    // the common case (it's the default and what every other tool but
    // explicit op-toggling produces), so it's left unmarked to keep a
    // normal stamp's row uncluttered; only the less-common Subtraction
    // case gets an explicit marker, the same "don't over-engineer, just
    // make the uncommon/surprising case legible at a glance" scope the
    // task's own guidance asks for.
    const QString op_suffix =
        stamp.op == SculptBrushOp::Subtraction ? " (Sub)" : "";
    history_list_->addItem(QString("%1: %2%3 @ (%4, %5, %6)")
                               .arg(i)
                               .arg(shape_name)
                               .arg(op_suffix)
                               .arg(stamp.center.x, 0, 'f', 2)
                               .arg(stamp.center.y, 0, 'f', 2)
                               .arg(stamp.center.z, 0, 'f', 2));
  }
  if (previous_row >= 0 && previous_row < history_list_->count()) {
    history_list_->setCurrentRow(previous_row);
  }
}

void ModellingWindow::shrink_undo_group_containing(size_t original_index) {
  // See this method's own header comment (main_window.h) for the full
  // reasoning -- walk undo_group_sizes_ from the back (most-recent stroke
  // first); `boundary` is the (exclusive) upper edge, in ORIGINAL
  // document_.stamps() positions, of whichever group is being examined --
  // starting at document_.stamps().size() itself (the list's current,
  // pre-removal length: the most-recent group always ends exactly there).
  size_t boundary = document_.stamps().size();
  for (int g = static_cast<int>(undo_group_sizes_.size()) - 1; g >= 0; --g) {
    const size_t group_size = static_cast<size_t>(undo_group_sizes_[g]);
    if (group_size == 0 || group_size > boundary) {
      // Malformed (shouldn't happen -- every push site keeps the running
      // sum equal to document_'s size) -- skip rather than underflow.
      continue;
    }
    const size_t group_start = boundary - group_size;
    if (original_index >= group_start && original_index < boundary) {
      if (group_size <= 1) {
        undo_group_sizes_.erase(undo_group_sizes_.begin() + g);
      } else {
        undo_group_sizes_[g] = static_cast<int>(group_size - 1);
      }
      return;
    }
    boundary = group_start;
  }
  // Not found -- undo_group_sizes_ doesn't fully cover document_ (shouldn't
  // happen given every entry point that adds to document_ also pushes a
  // matching group entry), but left as a silent no-op rather than
  // corrupting an unrelated group if it ever does.
}

void ModellingWindow::on_delete_edit_clicked() {
  if (!history_list_) {
    return;
  }
  const int row = history_list_->currentRow();
  if (row < 0 || static_cast<size_t>(row) >= document_.stamps().size()) {
    statusBar()->showMessage(
        "Select an edit in the history list first.", 4000);
    return;
  }

  // Must run BEFORE document_'s own removal below -- it reads document_.
  // stamps().size() as the ORIGINAL (pre-removal) list length to locate
  // which undo-group this position falls into (see its own comment).
  shrink_undo_group_containing(static_cast<size_t>(row));

  // GPU/engine-side first -- same "never claim more than actually
  // happened" discipline on_undo_triggered()/pickup_for_move() already
  // follow. If the engine's checkpoint-restore-and-replay fails (e.g. a
  // chunk-table slot genuinely ran out mid-replay), leave document_ (and
  // the undo-group bookkeeping just adjusted above) untouched rather than
  // drifting out of sync with a GPU state that never actually changed.
  if (!viewport_->delete_edit_at(row)) {
    statusBar()->showMessage(
        "Delete failed -- the engine could not revert/replay around that "
        "edit.",
        4000);
    return;
  }

  document_.remove_stamp_at(static_cast<size_t>(row));

  // A structural edit-list change invalidates any pending redo history --
  // the engine clears its own redo_stack_ for the identical reason inside
  // VulkanRendererBackend::delete_edit() (a stale redo_groups_ entry would
  // always re-append at document_'s CURRENT tail via on_redo_triggered(),
  // which is no longer where it originally sat relative to the rest of the
  // now-reshuffled history), so clearing here keeps both sides in
  // lockstep.
  redo_groups_.clear();

  update_status_label(); // Also refreshes history_list_ -- see its comment.
}

void ModellingWindow::on_move_edit_clicked() {
  if (!history_list_) {
    return;
  }
  const int row = history_list_->currentRow();
  if (row < 0 || static_cast<size_t>(row) >= document_.stamps().size()) {
    statusBar()->showMessage(
        "Select an edit in the history list first.", 4000);
    return;
  }
  // Unlike on_delete_edit_clicked(), nothing happens to document_/the
  // engine here at all -- this only ARMS the viewport's pending-move state
  // (see SceneViewport::begin_move_selected_edit()'s own comment). The
  // actual move_edit() call, and this window's matching document_ update,
  // happen later from on_edit_moved() once the user drags-and-releases in
  // the viewport. No undo-group bookkeeping to touch either (see this
  // method's own header comment, main_window.h) -- the edit isn't being
  // removed, just re-centered in place.
  viewport_->begin_move_selected_edit(row);
  statusBar()->showMessage(
      "Drag in the viewport and release to set this edit's new position.",
      4000);
}

void ModellingWindow::on_edit_moved(int index, glm::vec3 new_center) {
  // SceneViewport only emits this after renderer_sculpt_move_edit() already
  // confirmed the engine-side move happened -- same "GPU first" discipline
  // on_delete_edit_clicked()/pickup_for_move() already follow, so document_
  // can just update its own record unconditionally here.
  document_.update_stamp_center(static_cast<size_t>(index), new_center);
  update_status_label(); // Also refreshes history_list_ -- see its comment.
}

void ModellingWindow::on_crop_clicked() {
  // No ScopedRenderPause -- unlike Save/Load/Cutout below, this opens no
  // modal dialog, so the render timer stays running throughout.
  if (!viewport_->do_crop()) {
    statusBar()->showMessage(
        "Crop did nothing -- the engine hasn't initialized yet.", 4000);
    return;
  }
  statusBar()->showMessage(
      "Cropped: kept only the geometry inside the current brush shape. "
      "(Not undoable this pass.)",
      4000);
}

void ModellingWindow::on_cutout_clicked() {
  ScopedRenderPause pause(viewport_);
  QString path = QFileDialog::getSaveFileName(
      this, "Save Removed Portion (Cutout)", "cutout.sculptcut",
      "Sculpt cutout (*.sculptcut)", nullptr, QFileDialog::DontUseNativeDialog);
  if (path.isEmpty()) {
    return;
  }
  if (!viewport_->do_cutout(path)) {
    QMessageBox::warning(
        this, "Cutout Failed",
        QString("Could not write the removed portion to %1 -- the live "
               "sculpture was NOT modified.")
            .arg(path));
    return;
  }
  statusBar()->showMessage(
      QString("Cutout complete: removed portion saved to %1. The current "
             "session now keeps only the geometry inside the brush shape. "
             "(Not undoable this pass.)")
          .arg(path),
      6000);
}

void ModellingWindow::on_open_cutout_triggered() {
  ScopedRenderPause pause(viewport_);
  QString path = QFileDialog::getOpenFileName(
      this, "Open Cutout", QString(), "Sculpt cutout (*.sculptcut)", nullptr,
      QFileDialog::DontUseNativeDialog);
  if (path.isEmpty()) {
    return;
  }
  if (!viewport_->load_cutout_file(path)) {
    QMessageBox::warning(this, "Open Cutout Failed",
                         QString("Could not read %1").arg(path));
    return;
  }
  statusBar()->showMessage(
      QString("Loaded cutout file %1 (additive -- on top of whatever this "
             "session already has).")
          .arg(path),
      6000);
}

void ModellingWindow::on_save_triggered() {
  ScopedRenderPause pause(viewport_);
  // Pre-fills a sensible default filename (sculpt.stamps) when nothing's
  // been saved/loaded yet this session, so the dialog never opens with an
  // empty Name field the user has to fill in from scratch just to accept
  // the obvious default.
  QString suggested_path =
      current_path_.isEmpty() ? QString("sculpt.stamps") : current_path_;
  // DontUseNativeDialog: keeps the file picker a plain Qt/xcb window
  // instead of handing off to the desktop's native GTK file chooser --
  // deliberate, not just a default. The native GTK dialog pulls in a
  // second toolkit's own input/theming stack or portal dependency for a
  // one-off save/load picker this tool barely uses; staying in pure Qt
  // keeps behaviour identical across desktop environments (and avoids a
  // GTK dependency some minimal Linux setups this tool might run on won't
  // even have).
  QString path = QFileDialog::getSaveFileName(
      this, "Save Sculpt Stamps", suggested_path, "Sculpt stamps (*.stamps)",
      nullptr, QFileDialog::DontUseNativeDialog);
  if (path.isEmpty()) {
    return;
  }
  if (!document_.save(path.toStdString())) {
    QMessageBox::warning(this, "Save Failed",
                          QString("Could not write %1").arg(path));
    return;
  }
  current_path_ = path;
  update_status_label();
}

void ModellingWindow::on_load_triggered() {
  ScopedRenderPause pause(viewport_);
  // See on_save_triggered()'s comment -- same DontUseNativeDialog reasoning
  // applies symmetrically here.
  QString path = QFileDialog::getOpenFileName(
      this, "Load Sculpt Stamps", current_path_, "Sculpt stamps (*.stamps *.sculpt)",
      nullptr, QFileDialog::DontUseNativeDialog);
  if (path.isEmpty()) {
    return;
  }

  SculptDocument loaded;
  if (!loaded.load(path.toStdString())) {
    QMessageBox::warning(this, "Load Failed",
                          QString("Could not read %1").arg(path));
    return;
  }

  // There is no engine-side "clear the chunked field" API yet (see
  // renderer_frontend.h -- renderer_sculpt_stamp_shape() is the only
  // sculpt-volume write path exposed, and VulkanRaymarchShader's own
  // reset_chunked_field() is private to that class, not plumbed through
  // the renderer frontend). Building one is explicitly out of scope for
  // this milestone, so loading a file simply replays its stamps on TOP of
  // whatever is already stamped in this session -- it does not reproduce
  // a from-scratch scene if anything was stamped first. Opening a file
  // immediately after launching the tool (nothing stamped yet) round-
  // trips correctly; loading a *different* file into an already-sculpted
  // session will show both sets of stamps together.
  // Replays every stamp with its rotation and object grouping (and remaps
  // the file's object ids onto the engine's) -- see sculpt_shared.h.
  sculpt_shared::replay_document(loaded, /*one_by_one=*/true);
  for (size_t i = 0; i < loaded.stamps().size(); ++i) {
    // M3a: replay does NOT emit stamp_placed() (see its own
    // comment), so on_stroke_ended() never fires for it either --
    // undo_group_sizes_ needs an explicit entry per loaded stamp here, or
    // Undo after a Load would have no grouping information for any of
    // these and silently fall back to this session's default (1 stamp per
    // Ctrl+Z, see on_undo_triggered()). Pushing exactly one group of size
    // 1 per loaded stamp reproduces that same one-stamp-per-undo
    // granularity explicitly, matching pre-M3a per-stamp undo semantics
    // for anything that was never part of an actual on-screen drag.
    undo_group_sizes_.push_back(1);
  }

  // Each replay_stamp() call above goes through renderer_sculpt_stamp_
  // shape() -> VulkanRendererBackend::stamp_shape(), which clears the
  // engine's own redo_stack_ every time (a fresh stamp always invalidates
  // forward history -- see its own comment) -- so by the time this loop
  // finishes, the engine has no redo history left either. Clearing here
  // keeps the tool's mirror honest rather than leaving stale entries that
  // on_redo_triggered() would try to replay against a now-unrelated state.
  redo_groups_.clear();

  document_ = std::move(loaded);
  current_path_ = path;
  update_status_label();
}
