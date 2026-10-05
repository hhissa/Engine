#include "main_window.h"
#include "contents_tree_widget.h"
#include "scene_viewport.h"
#include <renderer/renderer_frontend.h>
#include <sdf_authoring.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QStackedWidget>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QItemSelectionModel>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QPushButton>
#include <QSignalBlocker>
#include <QInputDialog>
#include <QListWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {
// Fixed path renderer_load_scene() re-reads every time scene_ changes --
// see sync_viewport_scene(). Not the same as the user-chosen Save Scene...
// path; this one is purely an implementation detail of keeping the
// viewport live.
constexpr std::string_view kLivePreviewPath = "assets/scenes/.sdf_editor_live.sdf";

// How long request_viewport_resync() waits, after the LAST call in a
// burst, before actually running sync_viewport_scene_now() -- see that
// method's own comment. Long enough to coalesce a spinbox's rapid-fire
// valueChanged ticks (scrubbing/holding its arrows can easily fire
// several per 100ms) into one real sync; short enough that a single,
// isolated edit still feels effectively immediate.
constexpr int kSyncDebounceMs = 80;

// contents_tree_'s per-item data roles (see
// SdfEditorWindow::refresh_contents_list()) -- every item (layer or
// primitive) carries kLayerIndexRole; primitive items additionally carry
// kPrimitiveIndexRole (its position within that layer's primitives[]) and
// kPrimitiveNameRole (its stable SdfPrimitiveDef::name, the only thing that
// survives a drag-and-drop reparent -- see
// SdfEditorWindow::sync_layers_from_tree()).
constexpr int kLayerIndexRole = Qt::UserRole;

// properties_stack_'s two pages, in the order they're added (see the stack's
// own comment where it's built).
constexpr int kPrimitivePropertiesPage = 0;
constexpr int kLayerPropertiesPage = 1;
constexpr int kPrimitiveIndexRole = Qt::UserRole + 1;
constexpr int kPrimitiveNameRole = Qt::UserRole + 2;

// Stops viewport's render timer for as long as this guard is alive --
// construct one at the top of any handler that shows a modal dialog
// (QFileDialog/QColorDialog/QMessageBox all spin their own nested Qt event
// loop) and let it go out of scope when the handler returns. See
// SceneViewport::pause_rendering()'s own comment for why leaving the
// render timer running through a modal dialog was actually freezing the
// whole application, not just the 3D view.
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

// Turns an arbitrary source image filename into a safe assets/textures/
// basename (no extension) -- anything that isn't alphanumeric or '_'
// becomes '_', since the source file could be named with spaces/other
// punctuation glslc/the filesystem would rather not see echoed into an
// asset path.
std::string sanitize_texture_name(std::string name) {
  for (char &c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
      c = '_';
    }
  }
  return name.empty() ? std::string("texture") : name;
}

// Scans names for "<prefix><digits>" and returns one past the highest
// digits found (0 if none match) -- used after loading a file authored by
// this same tool (or a previous run of it) so next_layer_id_/next_light_id_
// resume above every id already on disk, instead of restarting at 0 and
// immediately colliding with a same-named survivor (see next_layer_id_'s
// comment in main_window.h for what that collision actually breaks).
u64 next_id_after(std::string_view prefix, const std::vector<std::string> &names) {
  u64 next = 0;
  for (const std::string &name : names) {
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    std::string digits = name.substr(prefix.size());
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), ::isdigit)) {
      continue;
    }
    u64 id = 0;
    try {
      id = std::stoull(digits);
    } catch (...) {
      continue;
    }
    next = std::max(next, id + 1);
  }
  return next;
}
} // namespace

namespace {
const char *primitive_type_label(SdfPrimitiveType type) {
  switch (type) {
  case SdfPrimitiveType::Sphere:
    return "Sphere";
  case SdfPrimitiveType::Box:
    return "Box";
  case SdfPrimitiveType::Plane:
    return "Plane";
  case SdfPrimitiveType::Torus:
    return "Torus";
  case SdfPrimitiveType::CappedCylinder:
    return "Capped Cylinder";
  case SdfPrimitiveType::CappedCone:
    return "Capped Cone";
  case SdfPrimitiveType::RoundBox:
    return "Round Box";
  case SdfPrimitiveType::BoxFrame:
    return "Box Frame";
  case SdfPrimitiveType::Octahedron:
    return "Octahedron";
  case SdfPrimitiveType::Pyramid:
    return "Pyramid";
  case SdfPrimitiveType::HexPrism:
    return "Hex Prism";
  case SdfPrimitiveType::RoundCone:
    return "Round Cone";
  case SdfPrimitiveType::Capsule:
    return "Capsule";
  case SdfPrimitiveType::Link:
    return "Link";
  case SdfPrimitiveType::Ellipsoid:
    return "Ellipsoid";
  }
  return "Sphere";
}

// Describes the "New Primitive" panel's shape for a given type: whether
// position/rotation apply (both are meaningless for Plane -- always the
// horizontal y=height plane, see GeometryConfig::plane()/add_plane()), and
// the label for each of param_spin_[0..3] to show (in order:
// params.x/y/z/extra_param -- see SdfPrimitiveDef's own comment for what
// each type actually does with them). 1 to 4 labels; param_spin_/
// param_label_ entries beyond however many a type uses are hidden (see
// update_field_enablement()).
struct PrimitiveTypeSpec {
  bool has_position;
  bool has_rotation;
  std::vector<const char *> param_labels;
  // Lower bound applied to every param_spin_[i] this type uses (see
  // update_field_enablement()). Defaults to 0.001 -- every param below is a
  // size/radius/extent that's meaningless at or below zero -- except Plane's
  // Height, which is a world-space Y position (see plane_sdf()) and is
  // routinely negative (a floor below the origin); GeometryConfig::plane()/
  // the .sdf parser/writer already accept any float here (no clamp on
  // either side), the UI's shared spinbox range was just never widened to
  // match.
  double param_min = 0.001;
};

PrimitiveTypeSpec type_spec_for(SdfPrimitiveType type) {
  switch (type) {
  case SdfPrimitiveType::Sphere:
    return {true, true, {"Radius"}};
  case SdfPrimitiveType::Box:
    return {true, true, {"Half-Extent X", "Half-Extent Y", "Half-Extent Z"}};
  case SdfPrimitiveType::Plane:
    return {false, false, {"Height"}, /*param_min=*/-100.0};
  case SdfPrimitiveType::Torus:
    return {true, true, {"Major Radius", "Minor Radius"}};
  case SdfPrimitiveType::CappedCylinder:
    return {true, true, {"Radius", "Half-Height"}};
  case SdfPrimitiveType::CappedCone:
    return {true, true, {"Half-Height", "Base Radius", "Tip Radius"}};
  case SdfPrimitiveType::RoundBox:
    return {true,
           true,
           {"Half-Extent X", "Half-Extent Y", "Half-Extent Z", "Corner Radius"}};
  case SdfPrimitiveType::BoxFrame:
    return {true,
           true,
           {"Half-Extent X", "Half-Extent Y", "Half-Extent Z", "Edge Thickness"}};
  case SdfPrimitiveType::Octahedron:
    return {true, true, {"Size"}};
  case SdfPrimitiveType::Pyramid:
    return {true, true, {"Height", "Base Half-Extent"}};
  case SdfPrimitiveType::HexPrism:
    return {true, true, {"Inradius", "Half-Height"}};
  case SdfPrimitiveType::RoundCone:
    return {true, true, {"Base Radius", "Tip Radius", "Half-Height"}};
  case SdfPrimitiveType::Capsule:
    return {true, true, {"Radius", "Half-Height"}};
  case SdfPrimitiveType::Link:
    return {true, true, {"Half-Length", "Inner Radius", "Thickness"}};
  case SdfPrimitiveType::Ellipsoid:
    return {true, true, {"Radius X", "Radius Y", "Radius Z"}};
  }
  return {true, true, {"Value"}};
}

// Wraps a fresh QFormLayout in a checkable QGroupBox and adds it to
// parent_layout, returning the form so the caller keeps building rows into
// it exactly like a plain, non-collapsible QFormLayout would -- splits what
// used to be one long flat "New Primitive"/"New Volumetric" form into
// several independently collapsible sections instead (clicking a section's
// title checkbox shows/hides its rows), so related fields (Transform,
// Repetition, Material & Texture, ...) read as one visually distinct group
// and a section nobody's using right now can be tucked away. Every section
// starts expanded by default (see `expanded`) so nothing that was always
// visible before this existed becomes hidden by default.
QFormLayout *add_collapsible_section(QVBoxLayout *parent_layout, const QString &title,
                                     bool expanded = true) {
  auto *box = new QGroupBox(title);
  box->setCheckable(true);
  box->setChecked(expanded);
  auto *box_layout = new QVBoxLayout(box);
  auto *content = new QWidget();
  content->setVisible(expanded);
  auto *content_form = new QFormLayout(content);
  content_form->setContentsMargins(0, 0, 0, 0);
  box_layout->addWidget(content);
  QObject::connect(box, &QGroupBox::toggled, content, &QWidget::setVisible);
  parent_layout->addWidget(box);
  return content_form;
}
} // namespace

SdfEditorWindow::SdfEditorWindow() {
  setWindowTitle("SDF Scene Editor");
  resize(720, 480);

  // See sync_debounce_timer_/request_viewport_resync()'s own comments --
  // constructed up front since field spinboxes (connected further below)
  // can fire before the rest of the window finishes building.
  sync_debounce_timer_ = new QTimer(this);
  sync_debounce_timer_->setSingleShot(true);
  connect(sync_debounce_timer_, &QTimer::timeout, this,
         &SdfEditorWindow::sync_viewport_scene_now);

  auto *central = new QWidget(this);
  auto *root_layout = new QHBoxLayout(central);

  // Left panel: scrollable list of primitive types to choose from.
  auto *left_panel = new QVBoxLayout();
  left_panel->addWidget(new QLabel("Primitive Type"));
  type_list_ = new QListWidget();
  // Added in exactly SdfPrimitiveType's own enum order -- row index and
  // enum value are used interchangeably throughout this file (see
  // populate_fields_from_selection()/on_add_clicked()/
  // update_field_enablement()).
  for (u32 i = 0; i <= static_cast<u32>(SdfPrimitiveType::Ellipsoid); ++i) {
    type_list_->addItem(primitive_type_label(static_cast<SdfPrimitiveType>(i)));
  }
  type_list_->setCurrentRow(0);
  type_list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  connect(type_list_, &QListWidget::currentItemChanged, this,
         &SdfEditorWindow::on_type_selection_changed);
  left_panel->addWidget(type_list_);
  root_layout->addLayout(left_panel, /*stretch=*/1);

  // Middle: the live rendered scene (see SceneViewport) -- right-drag
  // orbits, wheel zooms, left-click selects, left-click-drag on a gizmo
  // axis/ring moves/rotates the selected primitive (whichever the Move/
  // Rotate buttons below currently have active). The gizmo's lines are
  // drawn by the engine itself (see SceneViewport::draw_gizmo(), via
  // renderer_draw_line()) directly into the same native surface, not by a
  // separate overlay widget -- see scene_viewport.h's class comment for
  // why an overlay widget doesn't work here.
  viewport_ = new SceneViewport();
  connect(viewport_, &SceneViewport::selection_changed, this,
         &SdfEditorWindow::on_viewport_selection_changed);
  connect(viewport_, &SceneViewport::primitives_transformed, this,
         &SdfEditorWindow::on_viewport_primitives_transformed);
  connect(viewport_, &SceneViewport::gizmo_drag_started, this,
         &SdfEditorWindow::on_gizmo_drag_started);
  connect(viewport_, &SceneViewport::gizmo_drag_moved, this,
         &SdfEditorWindow::on_gizmo_drag_moved);
  connect(viewport_, &SceneViewport::gizmo_drag_ended, this,
         &SdfEditorWindow::on_gizmo_drag_ended);
  QWidget *viewport_container = QWidget::createWindowContainer(viewport_, central);
  // Keeps the swapchain from ever seeing a 0x0 extent (e.g. if the window
  // starts very small or a splitter gets dragged to its limit).
  viewport_container->setMinimumSize(320, 240);

  auto *middle_panel = new QVBoxLayout();
  auto *gizmo_mode_row = new QHBoxLayout();
  gizmo_mode_row->addWidget(new QLabel("Gizmo:"));
  move_mode_button_ = new QPushButton("Move");
  rotate_mode_button_ = new QPushButton("Rotate");
  move_mode_button_->setCheckable(true);
  rotate_mode_button_->setCheckable(true);
  move_mode_button_->setChecked(true); // matches SceneViewport's default (Translate)
  auto *gizmo_mode_group = new QButtonGroup(this);
  gizmo_mode_group->setExclusive(true);
  gizmo_mode_group->addButton(move_mode_button_);
  gizmo_mode_group->addButton(rotate_mode_button_);
  connect(move_mode_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_move_mode_clicked);
  connect(rotate_mode_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_rotate_mode_clicked);
  gizmo_mode_row->addWidget(move_mode_button_);
  gizmo_mode_row->addWidget(rotate_mode_button_);
  // Deliberately NOT in gizmo_mode_group -- it's an independent on/off
  // toggle, not a third mutually-exclusive gizmo mode.
  grid_button_ = new QPushButton("Show Grid");
  grid_button_->setCheckable(true);
  grid_button_->setChecked(true); // matches SceneViewport's default (shown)
  connect(grid_button_, &QPushButton::toggled, this,
         &SdfEditorWindow::on_show_grid_toggled);
  gizmo_mode_row->addWidget(grid_button_);
  // Also independent of gizmo_mode_group, same as Show Grid above.
  splat_button_ = new QPushButton("Splat Visibility");
  splat_button_->setToolTip(
      "Shade the chunked field's baked surface point cloud directly (the "
      "Dreams-style splat renderer) instead of marching a ray per pixel. "
      "Pixels no splat covers still march, so the image stays complete.");
  splat_button_->setCheckable(true);
  splat_button_->setChecked(false); // matches SceneViewport's default (off)
  connect(splat_button_, &QPushButton::toggled, this,
         &SdfEditorWindow::on_splat_visibility_toggled);
  gizmo_mode_row->addWidget(splat_button_);
  gizmo_mode_row->addStretch(/*stretch=*/1);
  middle_panel->addLayout(gizmo_mode_row);
  middle_panel->addWidget(viewport_container, /*stretch=*/1);
  root_layout->addLayout(middle_panel, /*stretch=*/3);

  // Right panel: how to join it in, its transform, its colour, and the
  // running scene contents.
  auto *right_panel = new QVBoxLayout();

  auto *primitives_tab = new QWidget();
  auto *primitives_layout = new QVBoxLayout(primitives_tab);

  // The property sections above the Scene Contents tree swap wholesale with
  // what that tree has selected: a primitive row shows the primitive's own
  // shape/material fields, a LAYER row shows the layer's -- its operation,
  // smoothness, and the repetition that repeats the whole layer, none of
  // which belong to any one primitive. Two pages of a stack rather than a
  // separate window: a layer's properties are edited in exactly the same
  // place, and with the same live-edit behaviour, as everything else here.
  // The tree itself and the buttons below it are outside the stack -- they
  // stay put whatever is selected.
  properties_stack_ = new QStackedWidget();
  auto *primitive_page = new QWidget();
  auto *primitive_page_layout = new QVBoxLayout(primitive_page);
  primitive_page_layout->setContentsMargins(0, 0, 0, 0);
  auto *layer_page = new QWidget();
  auto *layer_page_layout = new QVBoxLayout(layer_page);
  layer_page_layout->setContentsMargins(0, 0, 0, 0);
  properties_stack_->addWidget(primitive_page); // kPrimitivePropertiesPage
  properties_stack_->addWidget(layer_page);     // kLayerPropertiesPage
  primitives_layout->addWidget(properties_stack_);

  QFormLayout *form = add_collapsible_section(primitive_page_layout, "Layer");

  operation_combo_ = new QComboBox();
  operation_combo_->addItem("Union");
  operation_combo_->addItem("Subtraction");
  connect(operation_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
         this, &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Join Operation:", operation_combo_);

  smoothness_spin_ = new QDoubleSpinBox();
  smoothness_spin_->setRange(0.0, 10.0);
  smoothness_spin_->setSingleStep(0.05);
  connect(smoothness_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
         this, &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Smoothness:", smoothness_spin_);

  form = add_collapsible_section(primitive_page_layout, "Transform");

  pos_x_ = new QDoubleSpinBox();
  pos_y_ = new QDoubleSpinBox();
  pos_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin : {pos_x_, pos_y_, pos_z_}) {
    spin->setRange(-100.0, 100.0);
    spin->setSingleStep(0.1);
    spin->setToolTip(
        "Where this primitive sits inside its LAYER. For a layer that has "
        "not been moved (the usual case) that is world space; if the layer "
        "carries its own Transform, this is relative to it -- select the "
        "layer row to see or change that -- and moving the primitive to a "
        "different layer re-reads these numbers against that layer "
        "instead.");
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_live_edit_changed);
  }
  auto *pos_row = new QHBoxLayout();
  pos_row->addWidget(pos_x_);
  pos_row->addWidget(pos_y_);
  pos_row->addWidget(pos_z_);
  form->addRow("Position (x, y, z):", pos_row);

  rot_x_ = new QDoubleSpinBox();
  rot_y_ = new QDoubleSpinBox();
  rot_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin : {rot_x_, rot_y_, rot_z_}) {
    spin->setRange(-360.0, 360.0);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral("°"));
    spin->setToolTip(
        "Spins this primitive where it stands. Relative to its layer's own "
        "Rotation, the same way Position above is -- see that field.");
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_live_edit_changed);
  }
  auto *rot_row = new QHBoxLayout();
  rot_row->addWidget(rot_x_);
  rot_row->addWidget(rot_y_);
  rot_row->addWidget(rot_z_);
  form->addRow("Rotation (x, y, z):", rot_row);

  form = add_collapsible_section(primitive_page_layout, "Repetition", /*expanded=*/false);

  // Domain repetition (see https://iquilezles.org/articles/sdfrepetition/
  // and SdfPrimitiveDef::repetition_mode's comment) -- added in exactly
  // SdfRepetitionMode's own enum order, same "row index == enum value"
  // convention type_list_ uses (see update_field_enablement()/
  // populate_fields_from_selection()/apply_fields_to_primitive()).
  repetition_combo_ = new QComboBox();
  repetition_combo_->addItem("None");
  repetition_combo_->addItem("Infinite");
  repetition_combo_->addItem("Limited");
  repetition_combo_->addItem("Rotational");
  repetition_combo_->addItem("Rectangular");
  repetition_combo_->setToolTip(
      "Evaluates this shape at repeated copies of the sample point instead "
      "of just once.\n"
      "None: a plain, unrepeated primitive.\n"
      "Infinite: repeats forever every Repeat Cell unit along each axis "
      "whose cell value is > 0; an axis left at 0 doesn't repeat.\n"
      "Limited: like Infinite, but capped to Repeat Count copies per axis "
      "(a 3D box grid).\n"
      "Rotational: Repeat Count X evenly-spaced copies around this "
      "primitive's own local Y axis (combine with Rotation above to repeat "
      "around any axis).\n"
      "Rectangular: a 2D grid confined to the local XZ plane (Repeat Cell/"
      "Count X and Z; Y is left alone) -- the common 'tile the ground' "
      "case; use Limited for a full 3D grid instead.");
  connect(repetition_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
         this, &SdfEditorWindow::on_repetition_mode_changed);
  form->addRow("Repetition:", repetition_combo_);

  repeat_cell_x_ = new QDoubleSpinBox();
  repeat_cell_y_ = new QDoubleSpinBox();
  repeat_cell_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin : {repeat_cell_x_, repeat_cell_y_, repeat_cell_z_}) {
    spin->setRange(0.0, 100.0);
    spin->setSingleStep(0.1);
    spin->setValue(1.0);
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_live_edit_changed);
  }
  auto *repeat_cell_row = new QHBoxLayout();
  repeat_cell_row->addWidget(repeat_cell_x_);
  repeat_cell_row->addWidget(repeat_cell_y_);
  repeat_cell_row->addWidget(repeat_cell_z_);
  form->addRow("Repeat Cell (x, y, z):", repeat_cell_row);

  repeat_count_x_ = new QDoubleSpinBox();
  repeat_count_y_ = new QDoubleSpinBox();
  repeat_count_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin : {repeat_count_x_, repeat_count_y_, repeat_count_z_}) {
    spin->setDecimals(0);
    spin->setRange(1.0, 64.0);
    spin->setSingleStep(1.0);
    spin->setValue(1.0);
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_live_edit_changed);
  }
  auto *repeat_count_row = new QHBoxLayout();
  repeat_count_row->addWidget(repeat_count_x_);
  repeat_count_row->addWidget(repeat_count_y_);
  repeat_count_row->addWidget(repeat_count_z_);
  form->addRow("Repeat Count (x, y, z):", repeat_count_row);

  form = add_collapsible_section(primitive_page_layout, "Deformation", /*expanded=*/false);

  // Domain deformation (Inigo Quilez, https://iquilezles.org/articles/
  // distfunctions/ "Deforming" section) -- see SdfPrimitiveDef::twist/
  // bend/displace_amplitude/displace_frequency. All default to their
  // identity/no-op value, so a freshly added primitive renders unwarped
  // until one of these is actually touched.
  twist_spin_ = new QDoubleSpinBox();
  twist_spin_->setRange(-50.0, 50.0);
  twist_spin_->setSingleStep(0.1);
  twist_spin_->setValue(0.0);
  twist_spin_->setToolTip(
      "Radians of rotation per world-unit of local Y, around local Y -- "
      "twists the shape like a wrung-out cloth. 0 = no twist.");
  connect(twist_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Twist:", twist_spin_);

  bend_spin_ = new QDoubleSpinBox();
  bend_spin_->setRange(-50.0, 50.0);
  bend_spin_->setSingleStep(0.1);
  bend_spin_->setValue(0.0);
  bend_spin_->setToolTip(
      "Radians of rotation per world-unit along Bend Axis' first axis, "
      "swinging the shape toward its second -- applied after Twist. "
      "0 = no bend.");
  connect(bend_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Bend:", bend_spin_);

  // Which way the Bend above points. Rows are in SdfBendAxis' own enum
  // order, so currentIndex() casts straight to it -- same convention
  // repetition_combo_ follows for SdfRepetitionMode.
  bend_axis_combo_ = new QComboBox();
  bend_axis_combo_->addItem("X to Y");
  bend_axis_combo_->addItem("X to Z");
  bend_axis_combo_->addItem("Y to Z");
  bend_axis_combo_->addItem("Y to X");
  bend_axis_combo_->addItem("Z to X");
  bend_axis_combo_->addItem("Z to Y");
  bend_axis_combo_->setToolTip(
      "Which pair of local axes Bend warps, and which way round: \"X to "
      "Y\" (the default, and how Bend behaved before it had a direction) "
      "scales the angle by local X and swings the shape toward +Y, so a "
      "bar lying along X curves into an arc. Lets a bend be aimed without "
      "rotating the whole primitive to reach the axis you wanted.");
  connect(bend_axis_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
         this, &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Bend Axis:", bend_axis_combo_);

  displace_amplitude_spin_ = new QDoubleSpinBox();
  displace_amplitude_spin_->setRange(-10.0, 10.0);
  displace_amplitude_spin_->setSingleStep(0.01);
  displace_amplitude_spin_->setValue(0.0);
  displace_amplitude_spin_->setToolTip(
      "Added straight onto the shape's distance as amplitude * "
      "sin(f*x)*sin(f*y)*sin(f*z) (f = Displace Frequency) -- a rippled/"
      "bumpy surface perturbation. 0 = no displacement, regardless of "
      "frequency.");
  connect(displace_amplitude_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
         this, &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Displace Amplitude:", displace_amplitude_spin_);

  displace_frequency_spin_ = new QDoubleSpinBox();
  displace_frequency_spin_->setRange(0.0, 200.0);
  displace_frequency_spin_->setSingleStep(1.0);
  displace_frequency_spin_->setValue(20.0);
  displace_frequency_spin_->setToolTip(
      "The sin() rate in Displace Amplitude's formula -- higher means a "
      "finer ripple pattern. Has no effect while Displace Amplitude is 0.");
  connect(displace_frequency_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
         this, &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Displace Frequency:", displace_frequency_spin_);

  form = add_collapsible_section(primitive_page_layout, "Shape Parameters");

  // Generic per-type scalar parameters -- labeled/shown per the current
  // type's PrimitiveTypeSpec (see update_field_enablement()). Each also
  // gets an optional formula field (see param_expr_edit_) that, when
  // non-empty, overrides the spinbox for that slot -- a "parametric
  // attribute" (e.g. width = "0.1 + 0.1*p.y") instead of a fixed number.
  for (int i = 0; i < 4; ++i) {
    param_spin_[i] = new QDoubleSpinBox();
    param_spin_[i]->setRange(0.001, 100.0);
    param_spin_[i]->setSingleStep(0.1);
    param_spin_[i]->setValue(0.5);
    connect(param_spin_[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged),
           this, &SdfEditorWindow::on_live_edit_changed);

    param_expr_edit_[i] = new QLineEdit();
    param_expr_edit_[i]->setPlaceholderText("formula, e.g. 0.1 + 0.1*p.y");
    connect(param_expr_edit_[i], &QLineEdit::textChanged, this,
           &SdfEditorWindow::on_param_expr_changed);

    auto *param_row = new QHBoxLayout();
    param_row->addWidget(param_spin_[i]);
    param_row->addWidget(param_expr_edit_[i], /*stretch=*/1);

    param_label_[i] = new QLabel();
    form->addRow(param_label_[i], param_row);
  }

  form = add_collapsible_section(primitive_page_layout, "Material && Texture");

  colour_button_ = new QPushButton("Choose...");
  colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(colour_.name()));
  connect(colour_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_pick_colour_clicked);
  form->addRow("Colour:", colour_button_);

  texture_button_ = new QPushButton("Choose...");
  texture_clear_button_ = new QPushButton("Clear");
  texture_label_ = new QLabel("(none)");
  connect(texture_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_pick_texture_clicked);
  connect(texture_clear_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_clear_texture_clicked);
  auto *texture_row = new QHBoxLayout();
  texture_row->addWidget(texture_button_);
  texture_row->addWidget(texture_clear_button_);
  texture_row->addWidget(texture_label_, /*stretch=*/1);
  form->addRow("Texture:", texture_row);

  bump_map_button_ = new QPushButton("Choose...");
  bump_map_clear_button_ = new QPushButton("Clear");
  bump_map_label_ = new QLabel("(none)");
  bump_map_button_->setToolTip(
      "A separate texture sampled purely for surface-detail bump mapping, "
      "not colour. Leave unset for a flat surface -- bump mapping is no "
      "longer derived from the diffuse texture above.");
  connect(bump_map_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_pick_bump_map_clicked);
  connect(bump_map_clear_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_clear_bump_map_clicked);
  auto *bump_map_row = new QHBoxLayout();
  bump_map_row->addWidget(bump_map_button_);
  bump_map_row->addWidget(bump_map_clear_button_);
  bump_map_row->addWidget(bump_map_label_, /*stretch=*/1);
  form->addRow("Bump Map:", bump_map_row);

  bump_strength_spin_ = new QDoubleSpinBox();
  bump_strength_spin_->setRange(0.0, 10.0);
  bump_strength_spin_->setSingleStep(0.1);
  bump_strength_spin_->setValue(1.0); // matches Material::bump_strength's
                                     // engine-side default
  bump_strength_spin_->setToolTip(
      "How deep the bump map above reads. 1 is the fixed strength bump "
      "mapping used to be locked at; 0 flattens it without clearing the "
      "map, and larger values exaggerate the relief. No effect at all "
      "with no bump map set.");
  connect(bump_strength_spin_,
         QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Bump Strength:", bump_strength_spin_);

  texture_scale_spin_ = new QDoubleSpinBox();
  texture_scale_spin_->setRange(0.05, 50.0);
  texture_scale_spin_->setSingleStep(0.05);
  texture_scale_spin_->setValue(0.6); // matches Material::texture_scale's
                                     // engine-side default
  texture_scale_spin_->setToolTip(
      "World units one full repeat of the texture spans -- larger = the "
      "texture appears bigger on the surface.");
  connect(texture_scale_spin_,
         QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Texture Scale:", texture_scale_spin_);

  texture_offset_x_ = new QDoubleSpinBox();
  texture_offset_y_ = new QDoubleSpinBox();
  texture_offset_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin : {texture_offset_x_, texture_offset_y_, texture_offset_z_}) {
    spin->setRange(-100.0, 100.0);
    spin->setSingleStep(0.1);
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_live_edit_changed);
  }
  auto *texture_offset_row = new QHBoxLayout();
  texture_offset_row->addWidget(texture_offset_x_);
  texture_offset_row->addWidget(texture_offset_y_);
  texture_offset_row->addWidget(texture_offset_z_);
  form->addRow("Texture Offset (x, y, z):", texture_offset_row);

  texture_rotation_spin_ = new QDoubleSpinBox();
  texture_rotation_spin_->setRange(-360.0, 360.0);
  texture_rotation_spin_->setSingleStep(1.0);
  texture_rotation_spin_->setSuffix(QStringLiteral("°"));
  texture_rotation_spin_->setToolTip(
      "Rotates the texture pattern within each triplanar projection plane.");
  connect(texture_rotation_spin_,
         QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Texture Rotation:", texture_rotation_spin_);

  form = add_collapsible_section(primitive_page_layout, "Emissive && Rendering", /*expanded=*/false);

  emissive_colour_button_ = new QPushButton("Choose...");
  emissive_colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(emissive_colour_.name()));
  connect(emissive_colour_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_pick_emissive_colour_clicked);
  form->addRow("Emissive Colour:", emissive_colour_button_);

  emissive_intensity_spin_ = new QDoubleSpinBox();
  emissive_intensity_spin_->setRange(0.0, 100.0);
  emissive_intensity_spin_->setSingleStep(0.5);
  emissive_intensity_spin_->setValue(0.0); // matches Material::
                                          // emissive_intensity's
                                          // engine-side "off" default
  emissive_intensity_spin_->setToolTip(
      "0 = not emissive (a plain surface). Above 0, this primitive glows "
      "at that brightness regardless of scene lighting AND becomes a real "
      "point light source that illuminates everything else -- e.g. a "
      "light bulb or glowing panel. Meant for one deliberate light-shaped "
      "primitive, not every surface in the scene.");
  connect(emissive_intensity_spin_,
         QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Emissive Intensity:", emissive_intensity_spin_);

  pixelation_exempt_check_ = new QCheckBox("Pixelation Exempt");
  pixelation_exempt_check_->setToolTip(
      "If the game enables the pixelation post-process, this primitive "
      "stays crisp/full-resolution instead of pixelating along with "
      "everything else.");
  connect(pixelation_exempt_check_, &QCheckBox::toggled, this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("", pixelation_exempt_check_);

  casts_shadow_check_ = new QCheckBox("Casts Shadow");
  casts_shadow_check_->setChecked(true); // matches Material::casts_shadow's
                                        // engine-side default
  casts_shadow_check_->setToolTip(
      "Unticked, this primitive is still drawn and lit normally but every "
      "shadow ray passes straight through it -- it darkens nothing. For "
      "geometry that should read as an object without swallowing the room: "
      "glass, a light fixture's housing, an overhead grille. Bounced/GI "
      "light still sees it as ordinary geometry.");
  connect(casts_shadow_check_, &QCheckBox::toggled, this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("", casts_shadow_check_);

  // --- Surface + transmission ------------------------------------------
  roughness_spin_ = new QDoubleSpinBox();
  roughness_spin_->setRange(0.0, 1.0);
  roughness_spin_->setSingleStep(0.05);
  roughness_spin_->setValue(0.5); // matches MaterialDef::roughness
  roughness_spin_->setToolTip(
      "How sharp this surface's highlights are. 0 is mirror-sharp, 1 is "
      "fully matte. Polished plastic sits around 0.2, glass around 0.05, "
      "still water near 0.");
  connect(roughness_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
         this, &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Roughness:", roughness_spin_);

  // Transmissive is an explicit TOGGLE rather than "IOR above zero",
  // because the two are not the same range and conflating them is a trap.
  // MaterialDef stores 0 to mean opaque, but the physically meaningful
  // band for a transmissive IOR is [1, 3]: a value in (0, 1) describes
  // light leaving a denser medium, which at an air interface means a
  // near-total reflector (F0 approaches 1 as the IOR approaches 0, and
  // eta = 1/ior total-internal-reflects every ray). A single spin box
  // spanning 0 to 3 let one step off zero land squarely in that band, so
  // the first thing anyone saw on enabling glass was a mirror.
  transmissive_check_ = new QCheckBox("Transmissive");
  transmissive_check_->setToolTip(
      "Makes this primitive glass/liquid/clear plastic: rays pass through "
      "it and it is excluded from the voxel bake entirely, so moving it "
      "re-bakes nothing.");
  connect(transmissive_check_, &QCheckBox::toggled, this,
         &SdfEditorWindow::on_live_edit_changed);
  connect(transmissive_check_, &QCheckBox::toggled, this,
         &SdfEditorWindow::update_field_enablement);
  form->addRow("", transmissive_check_);

  ior_spin_ = new QDoubleSpinBox();
  // Starts at 1 (vacuum), not 0. The opaque case is the checkbox above.
  ior_spin_->setRange(1.0, 3.0);
  ior_spin_->setSingleStep(0.01);
  ior_spin_->setDecimals(3);
  ior_spin_->setValue(1.5);
  ior_spin_->setToolTip(
      "Index of refraction, only meaningful while Transmissive is on.\n\n"
      "Water 1.333, acrylic 1.49, glass 1.52, polycarbonate 1.585.");
  connect(ior_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("IOR:", ior_spin_);

  absorption_colour_button_ = new QPushButton("Absorption Tint");
  absorption_colour_button_->setToolTip(
      "The colour a slab of the reference thickness below lets through. "
      "This is what makes thick glass green at its edges and deep water "
      "blue -- authored as a colour you can judge rather than as an "
      "absorption coefficient. White means perfectly clear.");
  connect(absorption_colour_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_pick_absorption_colour_clicked);
  form->addRow("Absorption:", absorption_colour_button_);

  absorption_thickness_spin_ = new QDoubleSpinBox();
  absorption_thickness_spin_->setRange(0.001, 100.0);
  absorption_thickness_spin_->setSingleStep(0.01);
  absorption_thickness_spin_->setDecimals(3);
  absorption_thickness_spin_->setValue(0.1);
  absorption_thickness_spin_->setToolTip(
      "How deep a slab the absorption tint above describes, in world "
      "units. The renderer solves the two into a per-channel coefficient "
      "and applies it over the distance a ray actually travels inside the "
      "shape -- which an SDF knows exactly.");
  connect(absorption_thickness_spin_,
         QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Absorption Depth:", absorption_thickness_spin_);

  thin_walled_check_ = new QCheckBox("Thin-Walled");
  thin_walled_check_->setToolTip(
      "For parallel-walled glass thin enough that refraction would shift "
      "the image by less than a pixel -- a windowpane, a bottle wall. "
      "Skips the interior march entirely, which is both cheaper AND more "
      "accurate there than simulating a sub-pixel displacement. Leave it "
      "off for anything solid: a block, a lens, a tumbler's base.");
  connect(thin_walled_check_, &QCheckBox::toggled, this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("", thin_walled_check_);

  // --- The layer page: what a LAYER row's selection shows instead of all
  // of the above. Deliberately its own set of widgets rather than reusing
  // operation_combo_/smoothness_spin_ from the primitive page: those edit
  // the layer of whichever PRIMITIVE is selected, these edit the layer that
  // is itself selected, and both pages have to be able to hold their own
  // values at the same time.
  QFormLayout *layer_form = add_collapsible_section(layer_page_layout, "Layer");

  layer_operation_combo_ = new QComboBox();
  layer_operation_combo_->addItem("Union");
  layer_operation_combo_->addItem("Subtraction");
  layer_operation_combo_->setToolTip(
      "How every primitive in this layer folds into the scene built up so "
      "far: Union adds it, Subtraction carves it out. Applies to each "
      "primitive in the layer individually, not once to the layer as a "
      "whole -- so a subtraction layer with three shapes cuts three "
      "separate notches.");
  connect(layer_operation_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
         this, &SdfEditorWindow::on_layer_field_changed);
  layer_form->addRow("Join Operation:", layer_operation_combo_);

  layer_smoothness_spin_ = new QDoubleSpinBox();
  layer_smoothness_spin_->setRange(0.0, 10.0);
  layer_smoothness_spin_->setSingleStep(0.05);
  layer_smoothness_spin_->setToolTip(
      "Blend radius for this layer's fold, in world units. 0 is a hard "
      "edge; above 0 rounds the join between this layer's primitives and "
      "whatever they meet.");
  connect(layer_smoothness_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
         this, &SdfEditorWindow::on_layer_field_changed);
  layer_form->addRow("Smoothness:", layer_smoothness_spin_);

  layer_form = add_collapsible_section(layer_page_layout, "Layer Transform");

  layer_pos_x_ = new QDoubleSpinBox();
  layer_pos_y_ = new QDoubleSpinBox();
  layer_pos_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin : {layer_pos_x_, layer_pos_y_, layer_pos_z_}) {
    spin->setRange(-100.0, 100.0);
    spin->setSingleStep(0.1);
    spin->setToolTip(
        "Moves this WHOLE LAYER -- every primitive in it at once, keeping "
        "their arrangement -- without changing any of their own positions. "
        "Each primitive's Position is read relative to this.");
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_layer_field_changed);
  }
  auto *layer_pos_row = new QHBoxLayout();
  layer_pos_row->addWidget(layer_pos_x_);
  layer_pos_row->addWidget(layer_pos_y_);
  layer_pos_row->addWidget(layer_pos_z_);
  layer_form->addRow("Position (x, y, z):", layer_pos_row);

  layer_rot_x_ = new QDoubleSpinBox();
  layer_rot_y_ = new QDoubleSpinBox();
  layer_rot_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin : {layer_rot_x_, layer_rot_y_, layer_rot_z_}) {
    spin->setRange(-360.0, 360.0);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral("°"));
    spin->setToolTip(
        "Turns this WHOLE LAYER about its Position above, carrying every "
        "primitive in it around with their arrangement intact -- unlike a "
        "primitive's own Rotation, which spins each shape where it "
        "stands.\n"
        "A Plane in the layer is left alone: it is always horizontal and "
        "has no orientation to turn.\n"
        "Layer Repetition below still steps its copies along WORLD axes, "
        "not this rotated layer's -- the copies themselves are turned, the "
        "grid they sit on is not.");
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_layer_field_changed);
  }
  auto *layer_rot_row = new QHBoxLayout();
  layer_rot_row->addWidget(layer_rot_x_);
  layer_rot_row->addWidget(layer_rot_y_);
  layer_rot_row->addWidget(layer_rot_z_);
  layer_form->addRow("Rotation (x, y, z):", layer_rot_row);

  layer_form = add_collapsible_section(layer_page_layout, "Layer Repetition");

  // Same five modes, same cell/count meaning as a primitive's own
  // Repetition section -- added in SdfRepetitionMode's enum order, same
  // "row index == enum value" convention (see populate_layer_fields()/
  // apply_layer_fields()).
  layer_repetition_combo_ = new QComboBox();
  layer_repetition_combo_->addItem("None");
  layer_repetition_combo_->addItem("Infinite");
  layer_repetition_combo_->addItem("Limited");
  layer_repetition_combo_->addItem("Rotational");
  layer_repetition_combo_->addItem("Rectangular");
  layer_repetition_combo_->setToolTip(
      "Repeats this WHOLE LAYER -- every primitive in it at once, keeping "
      "their arrangement -- instead of repeating each shape around its own "
      "centre the way a primitive's own Repetition does. A table built "
      "from a top and four legs repeats as five tables here, and as five "
      "separately-spinning parts there.\n"
      "The fold happens in world space, before the layer's primitives are "
      "evaluated, so a subtraction layer repeats its cuts as a set too. "
      "The copies are laid out around the layer's own contents wherever "
      "they were authored -- it does not have to sit near the origin.\n"
      "None: the layer is used once, as authored.\n"
      "Infinite: forever, every Repeat Cell units along each axis whose "
      "cell is > 0. Costly -- nothing in the layer can be culled from any "
      "chunk of the bake again.\n"
      "Limited: as Infinite, capped to Repeat Count copies per axis.\n"
      "Rotational: Repeat Count X copies evenly spaced around the world Y "
      "axis -- a ring of whatever the layer holds.\n"
      "Rectangular: a grid on the world XZ plane (Cell/Count X and Z; Y is "
      "left alone).");
  connect(layer_repetition_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
         this, &SdfEditorWindow::on_layer_repetition_mode_changed);
  layer_form->addRow("Repetition:", layer_repetition_combo_);

  layer_repeat_cell_x_ = new QDoubleSpinBox();
  layer_repeat_cell_y_ = new QDoubleSpinBox();
  layer_repeat_cell_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin :
       {layer_repeat_cell_x_, layer_repeat_cell_y_, layer_repeat_cell_z_}) {
    spin->setRange(0.0, 100.0);
    spin->setSingleStep(0.1);
    spin->setValue(1.0);
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_layer_field_changed);
  }
  auto *layer_cell_row = new QHBoxLayout();
  layer_cell_row->addWidget(layer_repeat_cell_x_);
  layer_cell_row->addWidget(layer_repeat_cell_y_);
  layer_cell_row->addWidget(layer_repeat_cell_z_);
  layer_form->addRow("Repeat Cell (x, y, z):", layer_cell_row);

  layer_repeat_count_x_ = new QDoubleSpinBox();
  layer_repeat_count_y_ = new QDoubleSpinBox();
  layer_repeat_count_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin :
       {layer_repeat_count_x_, layer_repeat_count_y_, layer_repeat_count_z_}) {
    spin->setDecimals(0);
    spin->setRange(1.0, 64.0);
    spin->setSingleStep(1.0);
    spin->setValue(1.0);
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_layer_field_changed);
  }
  auto *layer_count_row = new QHBoxLayout();
  layer_count_row->addWidget(layer_repeat_count_x_);
  layer_count_row->addWidget(layer_repeat_count_y_);
  layer_count_row->addWidget(layer_repeat_count_z_);
  layer_form->addRow("Repeat Count (x, y, z):", layer_count_row);

  // Keeps both sections pinned to the top of the page: the layer page has
  // far fewer rows than the primitive one, and without this the stack
  // stretches them apart to fill the same height.
  layer_page_layout->addStretch(/*stretch=*/1);

  auto *add_row = new QHBoxLayout();
  auto *add_button = new QPushButton("Add Primitive");
  connect(add_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_add_clicked);
  add_row->addWidget(add_button);
  new_layer_button_ = new QPushButton("New Layer");
  new_layer_button_->setToolTip(
      "Starts an empty layer (Union, no smoothness) and selects it, so "
      "'Add Primitive' above adds into it -- the way to build up a layer "
      "that holds more than one primitive.");
  connect(new_layer_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_new_layer_clicked);
  add_row->addWidget(new_layer_button_);
  primitives_layout->addLayout(add_row);

  auto *primitive_clipboard_row = new QHBoxLayout();
  copy_primitives_button_ = new QPushButton("Copy Primitives");
  copy_primitives_button_->setToolTip(
      "Deep-copies every selected primitive onto an in-memory clipboard. "
      "Select as many as you like, across as many layers as you like; "
      "selecting a layer row copies every primitive in it.");
  connect(copy_primitives_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_copy_primitives_clicked);
  primitive_clipboard_row->addWidget(copy_primitives_button_);
  paste_primitives_button_ = new QPushButton("Paste Primitives");
  paste_primitives_button_->setEnabled(false); // enabled once something's copied
  paste_primitives_button_->setToolTip(
      "Adds a fresh copy of every copied primitive to the selected layer "
      "(select the layer row, or anything within it) -- with no layer "
      "selected they go into a new one. Each gets a newly generated unique "
      "name, so pasting repeatedly never collides with the original or an "
      "earlier paste.");
  connect(paste_primitives_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_paste_primitives_clicked);
  primitive_clipboard_row->addWidget(paste_primitives_button_);
  primitives_layout->addLayout(primitive_clipboard_row);

  primitives_layout->addWidget(new QLabel("Scene Contents"));
  contents_tree_ = new ContentsTreeWidget();
  contents_tree_->setHeaderHidden(true);
  connect(contents_tree_, &QTreeWidget::itemSelectionChanged, this,
         &SdfEditorWindow::on_contents_tree_selection_changed);
  connect(contents_tree_, &ContentsTreeWidget::primitives_reparented, this,
         &SdfEditorWindow::on_primitives_reparented);
  // Same rebuild either way -- sync_layers_from_tree() re-derives
  // scene_.layers from the tree's whole structure, top-level order
  // included, so a layer reorder needs no separate path.
  connect(contents_tree_, &ContentsTreeWidget::layers_reordered, this,
         &SdfEditorWindow::sync_layers_from_tree);
  primitives_layout->addWidget(contents_tree_, /*stretch=*/1);

  auto *remove_button = new QPushButton("Remove Selected");
  connect(remove_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_remove_clicked);
  primitives_layout->addWidget(remove_button);

  // Lights tab: mirrors the primitives tab's add/edit/remove pattern (see
  // populate_light_fields_from_selection()/apply_fields_to_light()), but
  // much simpler -- no operation/smoothness/rotation/gizmo, just a
  // type + direction-or-position + colour + intensity.
  auto *lights_tab = new QWidget();
  auto *lights_layout = new QVBoxLayout(lights_tab);

  auto *light_form_group = new QGroupBox("New Light");
  auto *light_form = new QFormLayout(light_form_group);

  light_type_combo_ = new QComboBox();
  light_type_combo_->addItem("Directional");
  light_type_combo_->addItem("Point");
  connect(light_type_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
         this, &SdfEditorWindow::on_light_type_changed);
  connect(light_type_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
         this, &SdfEditorWindow::on_light_field_changed);
  light_form->addRow("Type:", light_type_combo_);

  light_vec_x_ = new QDoubleSpinBox();
  light_vec_y_ = new QDoubleSpinBox();
  light_vec_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin : {light_vec_x_, light_vec_y_, light_vec_z_}) {
    spin->setRange(-100.0, 100.0);
    spin->setSingleStep(0.1);
  }
  // Set defaults *before* connecting valueChanged below -- lights_list_
  // doesn't exist yet at this point in the constructor, and
  // on_light_field_changed() dereferences it unconditionally, so a
  // setValue() call after connecting (with a value that actually differs
  // from the spinbox's own just-constructed default, so it isn't silently
  // suppressed) would crash immediately.
  light_vec_y_->setValue(0.7); // matches the engine's old default direction
  light_vec_z_->setValue(-0.6);
  for (QDoubleSpinBox *spin : {light_vec_x_, light_vec_y_, light_vec_z_}) {
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_light_field_changed);
  }
  auto *light_vec_row = new QHBoxLayout();
  light_vec_row->addWidget(light_vec_x_);
  light_vec_row->addWidget(light_vec_y_);
  light_vec_row->addWidget(light_vec_z_);
  light_vector_label_ = new QLabel("Direction (x, y, z):");
  light_form->addRow(light_vector_label_, light_vec_row);

  light_colour_button_ = new QPushButton("Choose...");
  light_colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(light_colour_.name()));
  connect(light_colour_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_pick_light_colour_clicked);
  light_form->addRow("Colour:", light_colour_button_);

  light_intensity_spin_ = new QDoubleSpinBox();
  light_intensity_spin_->setRange(0.0, 100.0);
  light_intensity_spin_->setSingleStep(0.1);
  light_intensity_spin_->setValue(0.85);
  connect(light_intensity_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
         this, &SdfEditorWindow::on_light_field_changed);
  light_form->addRow("Intensity:", light_intensity_spin_);

  lights_layout->addWidget(light_form_group);

  auto *add_light_button = new QPushButton("Add Light");
  connect(add_light_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_add_light_clicked);
  lights_layout->addWidget(add_light_button);

  lights_layout->addWidget(new QLabel("Lights"));
  lights_list_ = new QListWidget();
  lights_list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  connect(lights_list_, &QListWidget::currentItemChanged, this,
         &SdfEditorWindow::on_lights_list_selection_changed);
  lights_layout->addWidget(lights_list_, /*stretch=*/1);

  auto *remove_light_button = new QPushButton("Remove Selected");
  connect(remove_light_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_remove_light_clicked);
  lights_layout->addWidget(remove_light_button);

  // Volumetrics tab: mirrors the primitives tab's shape/transform/material
  // fields (type list, position, rotation, per-type params, colour,
  // texture, texture scale), minus operation/smoothness (a volumetric never
  // joins a layer -- it's never combined into the opaque scene at all) and
  // emissive/pixelation (meaningless for something that's never a solid
  // surface), plus a density field controlling how strongly it accumulates
  // glow per world unit a ray travels through it -- see
  // populate_volumetric_fields_from_selection()/apply_fields_to_volumetric().
  auto *volumetrics_tab = new QWidget();
  auto *volumetrics_root_layout = new QHBoxLayout(volumetrics_tab);

  auto *volumetric_type_panel = new QVBoxLayout();
  volumetric_type_panel->addWidget(new QLabel("Shape"));
  volumetric_type_list_ = new QListWidget();
  for (u32 i = 0; i <= static_cast<u32>(SdfPrimitiveType::Ellipsoid); ++i) {
    volumetric_type_list_->addItem(
        primitive_type_label(static_cast<SdfPrimitiveType>(i)));
  }
  volumetric_type_list_->setCurrentRow(static_cast<int>(SdfPrimitiveType::CappedCone));
  volumetric_type_list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  connect(volumetric_type_list_, &QListWidget::currentItemChanged, this,
         &SdfEditorWindow::on_volumetric_type_changed);
  volumetric_type_panel->addWidget(volumetric_type_list_);
  volumetrics_root_layout->addLayout(volumetric_type_panel, /*stretch=*/1);

  auto *volumetric_right_panel = new QVBoxLayout();
  QFormLayout *volumetric_form =
      add_collapsible_section(volumetric_right_panel, "Transform");

  volumetric_pos_x_ = new QDoubleSpinBox();
  volumetric_pos_y_ = new QDoubleSpinBox();
  volumetric_pos_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin :
       {volumetric_pos_x_, volumetric_pos_y_, volumetric_pos_z_}) {
    spin->setRange(-100.0, 100.0);
    spin->setSingleStep(0.1);
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_volumetric_field_changed);
  }
  auto *volumetric_pos_row = new QHBoxLayout();
  volumetric_pos_row->addWidget(volumetric_pos_x_);
  volumetric_pos_row->addWidget(volumetric_pos_y_);
  volumetric_pos_row->addWidget(volumetric_pos_z_);
  volumetric_form->addRow("Position (x, y, z):", volumetric_pos_row);

  volumetric_rot_x_ = new QDoubleSpinBox();
  volumetric_rot_y_ = new QDoubleSpinBox();
  volumetric_rot_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin :
       {volumetric_rot_x_, volumetric_rot_y_, volumetric_rot_z_}) {
    spin->setRange(-360.0, 360.0);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral("°"));
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_volumetric_field_changed);
  }
  auto *volumetric_rot_row = new QHBoxLayout();
  volumetric_rot_row->addWidget(volumetric_rot_x_);
  volumetric_rot_row->addWidget(volumetric_rot_y_);
  volumetric_rot_row->addWidget(volumetric_rot_z_);
  volumetric_form->addRow("Rotation (x, y, z):", volumetric_rot_row);

  volumetric_form = add_collapsible_section(volumetric_right_panel, "Shape Parameters");

  for (int i = 0; i < 4; ++i) {
    volumetric_param_spin_[i] = new QDoubleSpinBox();
    volumetric_param_spin_[i]->setRange(0.001, 100.0);
    volumetric_param_spin_[i]->setSingleStep(0.1);
    volumetric_param_spin_[i]->setValue(0.5);
    connect(volumetric_param_spin_[i],
           QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_volumetric_field_changed);

    volumetric_param_label_[i] = new QLabel();
    volumetric_form->addRow(volumetric_param_label_[i], volumetric_param_spin_[i]);
  }

  volumetric_form =
      add_collapsible_section(volumetric_right_panel, "Material, Texture && Glow");

  volumetric_colour_button_ = new QPushButton("Choose...");
  volumetric_colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(volumetric_colour_.name()));
  connect(volumetric_colour_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_pick_volumetric_colour_clicked);
  volumetric_form->addRow("Colour:", volumetric_colour_button_);

  volumetric_texture_button_ = new QPushButton("Choose...");
  volumetric_texture_clear_button_ = new QPushButton("Clear");
  volumetric_texture_label_ = new QLabel("(none)");
  connect(volumetric_texture_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_pick_volumetric_texture_clicked);
  connect(volumetric_texture_clear_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_clear_volumetric_texture_clicked);
  auto *volumetric_texture_row = new QHBoxLayout();
  volumetric_texture_row->addWidget(volumetric_texture_button_);
  volumetric_texture_row->addWidget(volumetric_texture_clear_button_);
  volumetric_texture_row->addWidget(volumetric_texture_label_, /*stretch=*/1);
  volumetric_form->addRow("Texture:", volumetric_texture_row);

  volumetric_texture_scale_spin_ = new QDoubleSpinBox();
  volumetric_texture_scale_spin_->setRange(0.05, 50.0);
  volumetric_texture_scale_spin_->setSingleStep(0.05);
  volumetric_texture_scale_spin_->setValue(0.6);
  volumetric_texture_scale_spin_->setToolTip(
      "World units one full repeat of the texture spans across the shaft's "
      "cross-section.");
  connect(volumetric_texture_scale_spin_,
         QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_volumetric_field_changed);
  volumetric_form->addRow("Texture Scale:", volumetric_texture_scale_spin_);

  volumetric_texture_offset_x_ = new QDoubleSpinBox();
  volumetric_texture_offset_y_ = new QDoubleSpinBox();
  volumetric_texture_offset_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin : {volumetric_texture_offset_x_, volumetric_texture_offset_y_,
                              volumetric_texture_offset_z_}) {
    spin->setRange(-100.0, 100.0);
    spin->setSingleStep(0.1);
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_volumetric_field_changed);
  }
  auto *volumetric_texture_offset_row = new QHBoxLayout();
  volumetric_texture_offset_row->addWidget(volumetric_texture_offset_x_);
  volumetric_texture_offset_row->addWidget(volumetric_texture_offset_y_);
  volumetric_texture_offset_row->addWidget(volumetric_texture_offset_z_);
  volumetric_form->addRow("Texture Offset (x, y, z):", volumetric_texture_offset_row);

  volumetric_texture_rotation_spin_ = new QDoubleSpinBox();
  volumetric_texture_rotation_spin_->setRange(-360.0, 360.0);
  volumetric_texture_rotation_spin_->setSingleStep(1.0);
  volumetric_texture_rotation_spin_->setSuffix(QStringLiteral("°"));
  connect(volumetric_texture_rotation_spin_,
         QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_volumetric_field_changed);
  volumetric_form->addRow("Texture Rotation:", volumetric_texture_rotation_spin_);

  volumetric_density_spin_ = new QDoubleSpinBox();
  volumetric_density_spin_->setRange(0.0, 20.0);
  volumetric_density_spin_->setSingleStep(0.1);
  volumetric_density_spin_->setValue(1.0);
  volumetric_density_spin_->setToolTip(
      "How strongly this shape accumulates its colour/texture per world "
      "unit a ray travels through it. It is never a solid surface -- rays "
      "always pass straight through -- higher just reads as a "
      "denser/brighter shaft.");
  connect(volumetric_density_spin_,
         QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_volumetric_field_changed);
  volumetric_form->addRow("Density:", volumetric_density_spin_);

  auto *add_volumetric_button = new QPushButton("Add Volumetric");
  connect(add_volumetric_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_add_volumetric_clicked);
  volumetric_right_panel->addWidget(add_volumetric_button);

  volumetric_right_panel->addWidget(new QLabel("Volumetrics"));
  volumetrics_list_ = new QListWidget();
  volumetrics_list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  connect(volumetrics_list_, &QListWidget::currentItemChanged, this,
         &SdfEditorWindow::on_volumetrics_list_selection_changed);
  volumetric_right_panel->addWidget(volumetrics_list_, /*stretch=*/1);

  auto *remove_volumetric_button = new QPushButton("Remove Selected");
  connect(remove_volumetric_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_remove_volumetric_clicked);
  volumetric_right_panel->addWidget(remove_volumetric_button);

  volumetrics_root_layout->addLayout(volumetric_right_panel, /*stretch=*/2);

  // --- Materials tab ---------------------------------------------------
  //
  // The scene's material library, as a thing you can look at. Before this
  // existed there was nothing to look at: a material was a filename
  // derived from the primitive panel's colour swatch, so the only way to
  // see what materials a scene had was to list a folder, and the only way
  // to change one everywhere was to select every primitive using it and
  // retype the values.
  auto *materials_tab = new QWidget();
  auto *materials_layout = new QVBoxLayout(materials_tab);

  materials_layout->addWidget(new QLabel("Scene Materials"));
  materials_list_ = new QListWidget();
  materials_list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  materials_list_->setToolTip(
      "Every material this scene defines, with how many primitives use "
      "each. Editing a material changes every primitive that references "
      "it.");
  connect(materials_list_, &QListWidget::currentItemChanged, this,
         &SdfEditorWindow::on_materials_list_selection_changed);
  materials_layout->addWidget(materials_list_, /*stretch=*/1);

  auto *material_buttons = new QHBoxLayout();
  auto *rename_material_button = new QPushButton("Rename");
  rename_material_button->setToolTip(
      "Renaming is free: a material is referenced by a stable id, not by "
      "its name.");
  connect(rename_material_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_rename_material_clicked);
  material_buttons->addWidget(rename_material_button);

  auto *duplicate_material_button = new QPushButton("Duplicate");
  duplicate_material_button->setToolTip(
      "A copy with the same values and a new identity -- the explicit way "
      "to branch a look, as opposed to editing a shared material and "
      "hoping only one primitive changes.");
  connect(duplicate_material_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_duplicate_material_clicked);
  material_buttons->addWidget(duplicate_material_button);

  auto *delete_material_button = new QPushButton("Delete");
  delete_material_button->setToolTip(
      "Only allowed once nothing references it -- a primitive can never be "
      "left pointing at a material that is gone.");
  connect(delete_material_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_delete_material_clicked);
  material_buttons->addWidget(delete_material_button);
  materials_layout->addLayout(material_buttons);

  auto *assign_material_button =
      new QPushButton("Assign to Selected Primitives");
  assign_material_button->setToolTip(
      "Points every primitive selected in the contents tree at this "
      "material.");
  connect(assign_material_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_assign_material_clicked);
  materials_layout->addWidget(assign_material_button);

  material_usage_label_ = new QLabel();
  material_usage_label_->setWordWrap(true);
  materials_layout->addWidget(material_usage_label_);

  auto *tabs = new QTabWidget();
  tabs_ = tabs;
  tabs->addTab(primitives_tab, "Primitives");
  // Remembered because it decides where a property edit LANDS -- see
  // on_live_edit_changed().
  materials_tab_index_ = tabs->addTab(materials_tab, "Materials");
  tabs->addTab(lights_tab, "Lights");
  tabs->addTab(volumetrics_tab, "Volumetrics");
  right_panel->addWidget(tabs, /*stretch=*/1);

  auto *ambient_row = new QHBoxLayout();
  ambient_row->addWidget(new QLabel("Ambient:"));
  ambient_spin_ = new QDoubleSpinBox();
  ambient_spin_->setRange(0.0, 1.0);
  ambient_spin_->setSingleStep(0.01);
  ambient_spin_->setValue(scene_.ambient);
  connect(ambient_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
         this, &SdfEditorWindow::on_ambient_changed);
  ambient_row->addWidget(ambient_spin_);
  right_panel->addLayout(ambient_row);

  // Scene-wide, so it sits out here beside Ambient rather than on any of
  // the tabs -- a skybox belongs to the scene, not to a primitive or a
  // light. Same Choose.../Clear/label shape the per-primitive texture
  // pickers use, and the same import behaviour (see
  // on_pick_skybox_clicked()).
  auto *skybox_row = new QHBoxLayout();
  skybox_row->addWidget(new QLabel("Skybox:"));
  skybox_button_ = new QPushButton("Choose...");
  skybox_clear_button_ = new QPushButton("Clear");
  skybox_label_ = new QLabel("(none)");
  const QString skybox_tooltip =
      "An equirectangular (lat/long) image drawn infinitely far behind the "
      "scene, and reflected off glossy surfaces at grazing angles -- not a "
      "six-face cubemap.\n"
      "Any image works, but one that is not 2:1 (twice as wide as it is "
      "tall) will look stretched: the width is wrapped once around the "
      "horizon and the height spans pole to pole.\n"
      "Cleared, the background falls back to the flat blue-black gradient.";
  skybox_button_->setToolTip(skybox_tooltip);
  skybox_clear_button_->setToolTip(skybox_tooltip);
  connect(skybox_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_pick_skybox_clicked);
  connect(skybox_clear_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_clear_skybox_clicked);
  skybox_row->addWidget(skybox_button_);
  skybox_row->addWidget(skybox_clear_button_);
  skybox_row->addWidget(skybox_label_, /*stretch=*/1);
  right_panel->addLayout(skybox_row);

  auto *file_row = new QHBoxLayout();
  auto *load_button = new QPushButton("Load Scene...");
  auto *save_button = new QPushButton("Save Scene...");
  connect(load_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_load_clicked);
  connect(save_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_save_clicked);
  file_row->addWidget(load_button);
  file_row->addWidget(save_button);
  right_panel->addLayout(file_row);

  root_layout->addLayout(right_panel, /*stretch=*/2);

  setCentralWidget(central);

  update_field_enablement();
  update_volumetric_field_enablement();
}

SdfEditorWindow::~SdfEditorWindow() {
  // Explicit, ahead of QMainWindow's own teardown -- Qt's widget-tree
  // destruction order doesn't guarantee viewport_'s C++ destructor runs
  // before its underlying native (XCB) window is destroyed, and
  // renderer_shutdown() needs that window to still exist while it runs.
  if (viewport_) {
    viewport_->shutdown_renderer();
  }
}

void SdfEditorWindow::on_type_selection_changed() { update_field_enablement(); }

void SdfEditorWindow::update_field_enablement() {
  int row = type_list_->currentRow();
  if (row < 0) {
    return;
  }
  PrimitiveTypeSpec spec = type_spec_for(static_cast<SdfPrimitiveType>(row));

  pos_x_->setEnabled(spec.has_position);
  pos_y_->setEnabled(spec.has_position);
  pos_z_->setEnabled(spec.has_position);
  rot_x_->setEnabled(spec.has_rotation);
  rot_y_->setEnabled(spec.has_rotation);
  rot_z_->setEnabled(spec.has_rotation);

  for (int i = 0; i < 4; ++i) {
    bool used = static_cast<size_t>(i) < spec.param_labels.size();
    param_label_[i]->setVisible(used);
    param_spin_[i]->setVisible(used);
    param_expr_edit_[i]->setVisible(used);
    if (used) {
      param_label_[i]->setText(QString::fromLatin1(spec.param_labels[i]) + ":");
      // Re-applied on every type switch (not just widened once for Plane)
      // so switching back to a size/radius type also restores the default
      // 0.001 floor -- see PrimitiveTypeSpec::param_min's own comment.
      param_spin_[i]->setRange(spec.param_min, 100.0);
      // A non-empty formula overrides the spinbox for this slot -- grey it
      // out to signal that.
      param_spin_[i]->setEnabled(param_expr_edit_[i]->text().isEmpty());
    }
  }

  // Repetition (see SdfRepetitionMode's comment) -- repeat_cell_*_/
  // repeat_count_*_ are shared across every mode, so which of the 6 is
  // actually meaningful (and therefore enabled, rather than hidden -- same
  // "stays visible but greyed" convention pos_*_/rot_*_ use for Plane
  // above) depends on the currently-chosen mode. Combo row index matches
  // SdfRepetitionMode's own enum order exactly.
  auto repetition_mode = static_cast<SdfRepetitionMode>(repetition_combo_->currentIndex());
  bool cell_relevant = repetition_mode == SdfRepetitionMode::Infinite ||
                      repetition_mode == SdfRepetitionMode::Limited ||
                      repetition_mode == SdfRepetitionMode::Rectangular;
  bool count_relevant = repetition_mode == SdfRepetitionMode::Limited ||
                       repetition_mode == SdfRepetitionMode::Rotational ||
                       repetition_mode == SdfRepetitionMode::Rectangular;
  bool is_rotational = repetition_mode == SdfRepetitionMode::Rotational;
  bool is_rectangular = repetition_mode == SdfRepetitionMode::Rectangular;

  // Transmission fields stay VISIBLE but greyed when the material is
  // opaque -- same convention pos_*_/rot_*_ use for a Plane, so the panel
  // never changes shape as you toggle things.
  const bool transmissive =
      transmissive_check_ != nullptr && transmissive_check_->isChecked();
  if (ior_spin_) {
    ior_spin_->setEnabled(transmissive);
    absorption_colour_button_->setEnabled(transmissive);
    absorption_thickness_spin_->setEnabled(transmissive);
    thin_walled_check_->setEnabled(transmissive);
  }

  repeat_cell_x_->setEnabled(cell_relevant);
  repeat_cell_y_->setEnabled(cell_relevant && !is_rectangular); // Rectangular locks Y
  repeat_cell_z_->setEnabled(cell_relevant);
  repeat_count_x_->setEnabled(count_relevant); // also n for Rotational
  repeat_count_y_->setEnabled(count_relevant && !is_rotational && !is_rectangular);
  repeat_count_z_->setEnabled(count_relevant && !is_rotational);
}

MaterialDef SdfEditorWindow::material_def_from_fields() const {
  MaterialDef def;
  def.base_colour = glm::vec4(
      static_cast<f32>(colour_.redF()), static_cast<f32>(colour_.greenF()),
      static_cast<f32>(colour_.blueF()), static_cast<f32>(colour_.alphaF()));
  def.base_map = texture_name_;
  def.uv_scale = static_cast<f32>(texture_scale_spin_->value());
  def.uv_offset = glm::vec3(static_cast<f32>(texture_offset_x_->value()),
                           static_cast<f32>(texture_offset_y_->value()),
                           static_cast<f32>(texture_offset_z_->value()));
  // The spin box is in degrees because that is what an author wants to
  // type; MaterialDef stores radians because that is what the shader
  // wants. This is the only place that conversion happens in either
  // direction (see populate_fields_from_material() for the inverse).
  def.uv_rotation =
      glm::radians(static_cast<f32>(texture_rotation_spin_->value()));
  def.bump_map = bump_map_name_;
  def.bump_strength = static_cast<f32>(bump_strength_spin_->value());
  def.emissive_colour = glm::vec3(
      static_cast<f32>(emissive_colour_.redF()),
      static_cast<f32>(emissive_colour_.greenF()),
      static_cast<f32>(emissive_colour_.blueF()));
  def.emissive_intensity =
      static_cast<f32>(emissive_intensity_spin_->value());
  def.casts_shadow = casts_shadow_check_->isChecked();
  def.pixelation_exempt = pixelation_exempt_check_->isChecked();
  def.roughness = static_cast<f32>(roughness_spin_->value());
  // 0 is the "opaque" sentinel; the spin box never produces it, so the
  // checkbox is the only thing that can.
  def.ior = transmissive_check_->isChecked()
                ? static_cast<f32>(ior_spin_->value())
                : 0.0f;
  def.absorption_tint = glm::vec3(
      static_cast<f32>(absorption_colour_.redF()),
      static_cast<f32>(absorption_colour_.greenF()),
      static_cast<f32>(absorption_colour_.blueF()));
  def.absorption_ref_thickness =
      static_cast<f32>(absorption_thickness_spin_->value());
  def.thin_walled = thin_walled_check_->isChecked();
  return def;
}

void SdfEditorWindow::populate_fields_from_material(const MaterialDef &def) {
  colour_ = QColor::fromRgbF(def.base_colour.r, def.base_colour.g,
                             def.base_colour.b, def.base_colour.a);
  colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(colour_.name()));

  texture_name_ = def.base_map;
  texture_label_->setText(texture_name_.empty()
                              ? QStringLiteral("(none)")
                              : QString::fromStdString(texture_name_));
  bump_map_name_ = def.bump_map;
  bump_map_label_->setText(bump_map_name_.empty()
                               ? QStringLiteral("(none)")
                               : QString::fromStdString(bump_map_name_));

  texture_scale_spin_->setValue(def.uv_scale);
  texture_offset_x_->setValue(def.uv_offset.x);
  texture_offset_y_->setValue(def.uv_offset.y);
  texture_offset_z_->setValue(def.uv_offset.z);
  texture_rotation_spin_->setValue(glm::degrees(def.uv_rotation));

  emissive_colour_ = QColor::fromRgbF(def.emissive_colour.r,
                                      def.emissive_colour.g,
                                      def.emissive_colour.b);
  emissive_colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(emissive_colour_.name()));
  emissive_intensity_spin_->setValue(def.emissive_intensity);

  bump_strength_spin_->setValue(def.bump_strength);
  pixelation_exempt_check_->setChecked(def.pixelation_exempt);
  casts_shadow_check_->setChecked(def.casts_shadow);

  roughness_spin_->setValue(def.roughness);
  transmissive_check_->setChecked(def.ior > 0.0f);
  // A def carrying an out-of-band IOR (a hand-edited scene, or one written
  // before the range was tightened) shows the nearest sane value rather
  // than putting the spin box somewhere it cannot represent.
  ior_spin_->setValue(def.ior >= 1.0f ? def.ior : 1.5f);
  absorption_colour_ = QColor::fromRgbF(def.absorption_tint.r,
                                        def.absorption_tint.g,
                                        def.absorption_tint.b);
  absorption_colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(absorption_colour_.name()));
  absorption_thickness_spin_->setValue(def.absorption_ref_thickness);
  thin_walled_check_->setChecked(def.thin_walled);
}

void SdfEditorWindow::on_pick_absorption_colour_clicked() {
  ScopedRenderPause pause(viewport_);
  QColor picked = QColorDialog::getColor(absorption_colour_, this,
                                        "Absorption Tint");
  if (!picked.isValid()) {
    return;
  }
  absorption_colour_ = picked;
  absorption_colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(absorption_colour_.name()));
  on_live_edit_changed();
}

MaterialId SdfEditorWindow::ensure_material_binding(MaterialId existing_id) {
  const MaterialDef fields = material_def_from_fields();

  // Editing IN PLACE is the default, and it is the entire point of the
  // library. If this primitive already references a material, and that
  // material is not shared with anything else, the author's edit means
  // "change this material" -- so change it, keeping the id, and every
  // reference to it (there is one) follows automatically.
  //
  // If it IS shared, the same edit means "make this one different", and
  // forking is the correct answer -- but only then, and only for this
  // primitive. That is the distinction the old filename scheme could not
  // draw, which is why it forked on every keystroke and accumulated 949
  // files.
  if (MaterialDef *existing = sdf_scene_find_material(scene_, existing_id)) {
    if (material_use_count(existing_id) <= 1) {
      const std::string previous_name = existing->display_name;
      const MaterialId previous_id = existing->id;
      MaterialDef updated = fields;
      updated.id = previous_id;
      updated.display_name = previous_name;
      // Preserve anything this build cannot interpret, exactly as the
      // reader/writer do.
      updated.unknown_keys = existing->unknown_keys;
      *existing = std::move(updated);
      refresh_material_list();
      return previous_id;
    }
  }

  return find_or_create_material(fields);
}

MaterialId SdfEditorWindow::find_or_create_material(const MaterialDef &values) {
  // Reuse by CONTENT, so picking a colour a material already in the scene
  // uses lands on that material rather than making a near-duplicate.
  const std::string key = material_def_content_key(values);
  for (const MaterialDef &existing : scene_.materials) {
    if (material_def_content_key(existing) == key) {
      return existing.id;
    }
  }

  MaterialDef created = values;
  created.id = material_id_generate();
  created.display_name = unique_material_name(material_def_suggest_name(values));
  const MaterialId id = created.id;
  scene_.materials.push_back(std::move(created));
  refresh_material_list();
  return id;
}

u32 SdfEditorWindow::material_use_count(MaterialId id) const {
  if (id == kInvalidMaterialId) {
    return 0;
  }
  u32 count = 0;
  for (const SdfLayerDef &layer : scene_.layers) {
    for (const SdfPrimitiveDef &primitive : layer.primitives) {
      if (primitive.material_id == id) {
        ++count;
      }
    }
  }
  for (const SdfVolumetricDef &volumetric : scene_.volumetrics) {
    if (volumetric.material_id == id) {
      ++count;
    }
  }
  return count;
}

std::string SdfEditorWindow::unique_material_name(std::string desired) const {
  // Display names are labels, not keys -- nothing breaks if two materials
  // share one. They are still disambiguated because a list with three
  // entries called "white" is not a usable list.
  auto taken = [this](const std::string &candidate) {
    for (const MaterialDef &material : scene_.materials) {
      if (material.display_name == candidate) {
        return true;
      }
    }
    return false;
  };
  if (!taken(desired)) {
    return desired;
  }
  for (int suffix = 2;; ++suffix) {
    std::string candidate = desired + " " + std::to_string(suffix);
    if (!taken(candidate)) {
      return candidate;
    }
  }
}

void SdfEditorWindow::refresh_material_list() {
  if (!materials_list_) {
    return;
  }
  const MaterialId previous = selected_material_id();

  QSignalBlocker blocker(materials_list_);
  materials_list_->clear();
  int restore_row = -1;
  for (const MaterialDef &material : scene_.materials) {
    const u32 uses = material_use_count(material.id);
    auto *item = new QListWidgetItem(
        QString("%1  (%2)")
            .arg(QString::fromStdString(material.display_name))
            .arg(uses));
    // A swatch, so the list reads as materials rather than as strings.
    item->setData(Qt::DecorationRole,
                  QColor::fromRgbF(material.base_colour.r,
                                   material.base_colour.g,
                                   material.base_colour.b));
    // The id, not the row: rows shift as materials are added and removed,
    // ids do not.
    item->setData(Qt::UserRole,
                  QString::fromStdString(material_id_to_string(material.id)));
    if (material.id == previous) {
      restore_row = materials_list_->count();
    }
    materials_list_->addItem(item);
  }
  if (restore_row >= 0) {
    materials_list_->setCurrentRow(restore_row);
  }
  update_material_usage_label();
}

MaterialId SdfEditorWindow::selected_material_id() const {
  if (!materials_list_) {
    return kInvalidMaterialId;
  }
  QListWidgetItem *item = materials_list_->currentItem();
  if (!item) {
    return kInvalidMaterialId;
  }
  MaterialId id = kInvalidMaterialId;
  material_id_from_string(item->data(Qt::UserRole).toString().toStdString(), id);
  return id;
}

void SdfEditorWindow::update_material_usage_label() {
  if (!material_usage_label_) {
    return;
  }
  const MaterialId id = selected_material_id();
  const MaterialDef *material = sdf_scene_find_material(scene_, id);
  if (!material) {
    material_usage_label_->setText(QString());
    return;
  }
  const u32 uses = material_use_count(id);
  material_usage_label_->setText(
      QString("id %1 -- used by %2 primitive%3.")
          .arg(QString::fromStdString(material_id_to_string(id)))
          .arg(uses)
          .arg(uses == 1 ? "" : "s"));
}

void SdfEditorWindow::on_materials_list_selection_changed() {
  update_material_usage_label();
  // Selecting a material loads it into the Primitives tab's property
  // widgets, so the one property panel serves both "edit this primitive's
  // material" and "edit this material". Nothing is written back until a
  // widget actually changes.
  const MaterialDef *material =
      sdf_scene_find_material(scene_, selected_material_id());
  if (!material) {
    return;
  }
  populating_fields_ = true;
  populate_fields_from_material(*material);
  populating_fields_ = false;
}

void SdfEditorWindow::apply_fields_to_selected_material() {
  MaterialDef *material = sdf_scene_find_material(scene_, selected_material_id());
  if (!material) {
    return;
  }
  // Values are replaced wholesale; identity is not. Keeping the id is the
  // entire mechanism -- every primitive already references it, so they all
  // pick the change up with no re-binding at all, and because the id did
  // not change, reconcile_scene() sees an ordinary value edit rather than
  // a destructive material swap.
  MaterialDef updated = material_def_from_fields();
  updated.id = material->id;
  updated.display_name = material->display_name;
  updated.unknown_keys = material->unknown_keys;
  *material = std::move(updated);

  refresh_material_list();
  request_viewport_resync();
}

void SdfEditorWindow::on_rename_material_clicked() {
  MaterialDef *material = sdf_scene_find_material(scene_, selected_material_id());
  if (!material) {
    return;
  }
  bool accepted = false;
  const QString name = QInputDialog::getText(
      this, "Rename Material", "Name:", QLineEdit::Normal,
      QString::fromStdString(material->display_name), &accepted);
  if (!accepted || name.trimmed().isEmpty()) {
    return;
  }
  // A rename touches nothing but the label -- no primitive is re-bound, no
  // material is re-resolved, and the renderer is not even told, because
  // the resolved content of every binding is unchanged.
  material->display_name = unique_material_name(name.trimmed().toStdString());
  refresh_material_list();
}

void SdfEditorWindow::on_duplicate_material_clicked() {
  const MaterialDef *source =
      sdf_scene_find_material(scene_, selected_material_id());
  if (!source) {
    return;
  }
  MaterialDef copy = *source;
  copy.id = material_id_generate();
  copy.display_name = unique_material_name(source->display_name);
  scene_.materials.push_back(std::move(copy));
  refresh_material_list();
}

void SdfEditorWindow::on_delete_material_clicked() {
  const MaterialId id = selected_material_id();
  const MaterialDef *material = sdf_scene_find_material(scene_, id);
  if (!material) {
    return;
  }
  const u32 uses = material_use_count(id);
  if (uses > 0) {
    // Refused rather than cascaded: the old scheme could not produce a
    // dangling reference (primitives pointed at files, which always
    // existed), so this failure mode is new and worth being explicit
    // about instead of silently reassigning someone's geometry.
    QMessageBox::information(
        this, "Material In Use",
        QString("'%1' is used by %2 primitive%3. Assign them to another "
                "material first.")
            .arg(QString::fromStdString(material->display_name))
            .arg(uses)
            .arg(uses == 1 ? "" : "s"));
    return;
  }
  scene_.materials.erase(scene_.materials.begin() +
                         (material - scene_.materials.data()));
  refresh_material_list();
}

void SdfEditorWindow::on_assign_material_clicked() {
  const MaterialId id = selected_material_id();
  if (sdf_scene_find_material(scene_, id) == nullptr) {
    return;
  }
  const std::vector<PrimitiveRef> selection = tree_selected_primitives();
  if (selection.empty()) {
    return;
  }
  bool changed = false;
  for (const PrimitiveRef &ref : selection) {
    if (ref.layer_index < 0 ||
        ref.layer_index >= static_cast<int>(scene_.layers.size())) {
      continue;
    }
    SdfLayerDef &layer = scene_.layers[ref.layer_index];
    if (ref.primitive_index < 0 ||
        ref.primitive_index >= static_cast<int>(layer.primitives.size())) {
      continue;
    }
    SdfPrimitiveDef &primitive = layer.primitives[ref.primitive_index];
    if (primitive.material_id == id) {
      continue;
    }
    primitive.material_id = id;
    // Assigning a library material also drops any per-primitive overrides:
    // they were tweaks relative to a DIFFERENT material and would mean
    // something else here.
    primitive.material_overrides.clear();
    primitive.material_name.clear();
    changed = true;
  }
  if (changed) {
    refresh_material_list();
    sync_viewport_scene();
  }
}

void SdfEditorWindow::on_add_clicked() {
  int row = type_list_->currentRow();
  if (row < 0) {
    return;
  }
  SdfPrimitiveType type = static_cast<SdfPrimitiveType>(row);
  PrimitiveTypeSpec spec = type_spec_for(type);

  // A brand-new primitive never edits an existing material in place --
  // there is nothing yet bound to edit -- so this always resolves to
  // "reuse the library material with these values, or add one".
  const MaterialId material_id = find_or_create_material(material_def_from_fields());

  // If a layer is currently active (see active_layer_index_ -- set by
  // selecting one of its primitives, the layer row itself, or clicking New
  // Layer), add into it instead of always starting a fresh layer -- the
  // way to build up a layer that holds more than one primitive. Its own
  // operation/smoothness (set when it was created) are left alone here;
  // the Join Operation/Smoothness fields below only apply when they're
  // about to define a brand-new layer.
  SdfLayerDef *layer_ptr;
  if (active_layer_index_ >= 0 &&
      active_layer_index_ < static_cast<int>(scene_.layers.size())) {
    layer_ptr = &scene_.layers[active_layer_index_];
  } else {
    SdfLayerOperation operation = operation_combo_->currentIndex() == 1
                                      ? SdfLayerOperation::Subtraction
                                      : SdfLayerOperation::Union;
    f32 smoothness = static_cast<f32>(smoothness_spin_->value());
    std::string layer_name = "layer" + std::to_string(next_layer_id_++);
    layer_ptr = &add_layer(scene_, layer_name, operation, smoothness);
  }
  SdfLayerDef &layer = *layer_ptr;
  // Globally unique regardless of which layer it lands in -- a layer can
  // now hold more than one primitive, so the old "<layer_name>_primitive"
  // convention (exactly one per layer) would collide the moment a second
  // primitive joined the same layer.
  std::string primitive_name = "primitive" + std::to_string(next_primitive_id_++);

  glm::vec3 position =
      spec.has_position
          ? glm::vec3(static_cast<f32>(pos_x_->value()),
                     static_cast<f32>(pos_y_->value()),
                     static_cast<f32>(pos_z_->value()))
          : glm::vec3(0.0f);
  glm::vec3 rotation =
      spec.has_rotation
          ? glm::radians(glm::vec3(static_cast<f32>(rot_x_->value()),
                                  static_cast<f32>(rot_y_->value()),
                                  static_cast<f32>(rot_z_->value())))
          : glm::vec3(0.0f);

  f32 raw_params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (size_t i = 0; i < spec.param_labels.size() && i < 4; ++i) {
    raw_params[i] = static_cast<f32>(param_spin_[i]->value());
  }
  glm::vec3 params(raw_params[0], raw_params[1], raw_params[2]);
  f32 extra_param = raw_params[3];

  SdfPrimitiveDef *added;
  if (type == SdfPrimitiveType::Sphere) {
    added = &add_sphere(layer, primitive_name, position, rotation, params.x,
                       std::string{});
  } else if (type == SdfPrimitiveType::Box) {
    added = &add_box(layer, primitive_name, position, rotation, params,
                    std::string{});
  } else if (type == SdfPrimitiveType::Plane) {
    added = &add_plane(layer, primitive_name, params.x,
                      std::string{}); // params.x = height
  } else {
    added = &add_primitive(layer, primitive_name, type, position, rotation,
                          params, extra_param, std::string{});
  }

  // The material binding is by id, not by the legacy .kmt name the add_*
  // helpers still accept for code-driven scenes.
  added->material_id = material_id;

  // "Parametric attribute" formulas -- only for slots this type actually
  // uses (see spec.param_labels above); an empty string means "no formula,
  // use the plain constant" (see SdfPrimitiveDef::param_expressions).
  for (size_t i = 0; i < spec.param_labels.size() && i < 4; ++i) {
    added->param_expressions[i] = param_expr_edit_[i]->text().toStdString();
  }

  added->repetition_mode =
      static_cast<SdfRepetitionMode>(repetition_combo_->currentIndex());
  added->repetition_cell = glm::vec3(static_cast<f32>(repeat_cell_x_->value()),
                                     static_cast<f32>(repeat_cell_y_->value()),
                                     static_cast<f32>(repeat_cell_z_->value()));
  added->repetition_count = glm::vec3(static_cast<f32>(repeat_count_x_->value()),
                                      static_cast<f32>(repeat_count_y_->value()),
                                      static_cast<f32>(repeat_count_z_->value()));

  refresh_contents_list();
  sync_viewport_scene();
}

void SdfEditorWindow::on_remove_clicked() {
  QList<QTreeWidgetItem *> selected = contents_tree_->selectedItems();
  if (selected.isEmpty()) {
    return;
  }

  // A selected layer row removes the whole layer (every primitive in it,
  // even ones not separately selected); a selected primitive row removes
  // just that primitive, identified by its stable name (see
  // refresh_contents_list()'s comment) since indices may span several
  // layers at once here.
  std::set<int> whole_layers_to_remove;
  std::set<std::string> primitive_names_to_remove;
  for (QTreeWidgetItem *item : selected) {
    if (!item->parent()) {
      whole_layers_to_remove.insert(item->data(0, kLayerIndexRole).toInt());
    } else {
      primitive_names_to_remove.insert(
          item->data(0, kPrimitiveNameRole).toString().toStdString());
    }
  }

  std::vector<SdfLayerDef> kept_layers;
  kept_layers.reserve(scene_.layers.size());
  for (int i = 0; i < static_cast<int>(scene_.layers.size()); ++i) {
    if (whole_layers_to_remove.count(i)) {
      continue;
    }
    SdfLayerDef &layer = scene_.layers[i];
    std::vector<SdfPrimitiveDef> kept_primitives;
    kept_primitives.reserve(layer.primitives.size());
    for (SdfPrimitiveDef &primitive : layer.primitives) {
      if (!primitive_names_to_remove.count(primitive.name)) {
        kept_primitives.push_back(std::move(primitive));
      }
    }
    layer.primitives = std::move(kept_primitives);
    kept_layers.push_back(std::move(layer));
  }
  scene_.layers = std::move(kept_layers);

  refresh_contents_list();
  sync_viewport_scene();
  // Use counts changed, and a material may have become unreferenced (and
  // therefore deletable).
  refresh_material_list();
  // Layer/primitive indices may have shifted (or no longer exist at all) --
  // any previous selection is potentially stale/wrong now.
  viewport_->set_selection({});
  active_layer_index_ = -1;
}

void SdfEditorWindow::on_new_layer_clicked() {
  std::string layer_name = "layer" + std::to_string(next_layer_id_++);
  // Always Union/no-smoothness regardless of whatever the "New Primitive"
  // form's Join Operation/Smoothness fields currently show -- those fields
  // describe what to create for on_add_clicked()'s fallback path, not this
  // explicit, blank action (see request: "each layer is by default
  // unioned").
  add_layer(scene_, layer_name, SdfLayerOperation::Union, 0.0f);

  refresh_contents_list();
  sync_viewport_scene();

  // Select the new (empty) layer row -- on_contents_tree_selection_changed()
  // then sets active_layer_index_ to it, so the very next Add Primitive
  // click adds into it instead of starting yet another layer.
  for (int i = 0; i < contents_tree_->topLevelItemCount(); ++i) {
    QTreeWidgetItem *layer_item = contents_tree_->topLevelItem(i);
    if (layer_item->data(0, kLayerIndexRole).toInt() ==
        static_cast<int>(scene_.layers.size()) - 1) {
      contents_tree_->setCurrentItem(layer_item);
      break;
    }
  }
}

void SdfEditorWindow::on_copy_primitives_clicked() {
  QList<QTreeWidgetItem *> selected = contents_tree_->selectedItems();
  if (selected.isEmpty()) {
    return;
  }

  // A primitive row names itself; a layer row names everything in it, so
  // "copy this whole layer's contents" is still one click. A std::set both
  // dedupes (selecting a primitive AND its layer row names it twice) and
  // gives ascending (layer, primitive) order for free -- which is the order
  // they will be pasted back in, so a multi-select paste preserves the
  // scene's own ordering rather than the order rows happened to be clicked.
  std::set<std::pair<int, int>> refs;
  for (QTreeWidgetItem *item : selected) {
    if (item->parent()) {
      refs.emplace(item->parent()->data(0, kLayerIndexRole).toInt(),
                   item->data(0, kPrimitiveIndexRole).toInt());
      continue;
    }
    const int layer_index = item->data(0, kLayerIndexRole).toInt();
    if (layer_index < 0 || layer_index >= static_cast<int>(scene_.layers.size())) {
      continue;
    }
    const auto &primitives = scene_.layers[layer_index].primitives;
    for (int i = 0; i < static_cast<int>(primitives.size()); ++i) {
      refs.emplace(layer_index, i);
    }
  }

  // Only overwrite the clipboard once we know there is something to put in
  // it -- a selection of nothing but empty layer rows should leave whatever
  // was copied earlier alone, exactly as an empty selection does.
  std::vector<SdfPrimitiveDef> copied;
  copied.reserve(refs.size());
  for (const auto &[layer_index, primitive_index] : refs) {
    if (layer_index < 0 || layer_index >= static_cast<int>(scene_.layers.size())) {
      continue;
    }
    const auto &primitives = scene_.layers[layer_index].primitives;
    if (primitive_index < 0 ||
        primitive_index >= static_cast<int>(primitives.size())) {
      continue;
    }
    copied.push_back(primitives[primitive_index]); // deep copy -- a
                                                   // SdfPrimitiveDef owns
                                                   // its params outright
  }
  if (copied.empty()) {
    return;
  }

  primitive_clipboard_ = std::move(copied);
  paste_primitives_button_->setEnabled(true);
}

void SdfEditorWindow::on_paste_primitives_clicked() {
  if (primitive_clipboard_.empty()) {
    return;
  }

  // Into the SELECTED layer -- active_layer_index_ is already exactly that
  // notion (a lone layer row, or a selection that lies entirely within one
  // layer; -1 when the selection spans several or there is none). With no
  // layer to target, start one, mirroring on_add_clicked()'s own fallback
  // and its use of the New Primitive form's Join Operation/Smoothness for
  // the layer it creates -- pasting should never silently do nothing.
  int target_layer;
  if (active_layer_index_ >= 0 &&
      active_layer_index_ < static_cast<int>(scene_.layers.size())) {
    target_layer = active_layer_index_;
  } else {
    SdfLayerOperation operation = operation_combo_->currentIndex() == 1
                                      ? SdfLayerOperation::Subtraction
                                      : SdfLayerOperation::Union;
    f32 smoothness = static_cast<f32>(smoothness_spin_->value());
    std::string layer_name = "layer" + std::to_string(next_layer_id_++);
    add_layer(scene_, layer_name, operation, smoothness);
    target_layer = static_cast<int>(scene_.layers.size()) - 1;
  }

  // Deep copy, then rename every pasted primitive to a fresh, globally
  // unique name -- GeometrySystem::acquire() (engine-side) keys purely off
  // name, so pasting the clipboard's names verbatim would silently bump the
  // *originals'* reference counts instead of registering new geometry,
  // discarding whichever position/params the pasted copy was actually given
  // (see on_add_clicked()'s own comment on next_primitive_id_ for the exact
  // same hazard). Reusing next_primitive_id_ -- the same monotonic counter
  // on_add_clicked() already draws from -- keeps every name in the scene
  // unique regardless of whether it came from Add or Paste.
  auto &target_primitives = scene_.layers[target_layer].primitives;
  const int first_pasted_index = static_cast<int>(target_primitives.size());
  for (const SdfPrimitiveDef &copied : primitive_clipboard_) {
    SdfPrimitiveDef pasted = copied;
    pasted.name = "primitive" + std::to_string(next_primitive_id_++);
    target_primitives.push_back(std::move(pasted));
  }

  refresh_contents_list();
  sync_viewport_scene();

  // Select every newly pasted primitive row, so the gizmo lands on the
  // copies rather than the originals and they can be dragged straight off
  // the things they are sitting exactly on top of.
  contents_tree_->clearSelection();
  QTreeWidgetItem *first_pasted_item = nullptr;
  for (int i = 0; i < contents_tree_->topLevelItemCount(); ++i) {
    QTreeWidgetItem *layer_item = contents_tree_->topLevelItem(i);
    if (layer_item->data(0, kLayerIndexRole).toInt() != target_layer) {
      continue;
    }
    for (int j = 0; j < layer_item->childCount(); ++j) {
      QTreeWidgetItem *primitive_item = layer_item->child(j);
      if (primitive_item->data(0, kPrimitiveIndexRole).toInt() <
          first_pasted_index) {
        continue;
      }
      primitive_item->setSelected(true);
      if (!first_pasted_item) {
        first_pasted_item = primitive_item;
      }
    }
    break;
  }
  if (first_pasted_item) {
    // NoUpdate -- setCurrentItem() otherwise re-collapses the selection down
    // to just this one item under ExtendedSelection.
    contents_tree_->setCurrentItem(first_pasted_item, 0,
                                    QItemSelectionModel::NoUpdate);
  }
}

void SdfEditorWindow::on_pick_colour_clicked() {
  ScopedRenderPause pause(viewport_);
  QColor picked = QColorDialog::getColor(colour_, this, "Select Colour",
                                         QColorDialog::ShowAlphaChannel);
  if (picked.isValid()) {
    colour_ = picked;
    colour_button_->setStyleSheet(
        QString("background-color: %1;").arg(colour_.name()));
    on_live_edit_changed(); // apply immediately if a primitive is selected
  }
}

void SdfEditorWindow::on_pick_emissive_colour_clicked() {
  ScopedRenderPause pause(viewport_);
  QColor picked =
      QColorDialog::getColor(emissive_colour_, this, "Select Emissive Colour");
  if (picked.isValid()) {
    emissive_colour_ = picked;
    emissive_colour_button_->setStyleSheet(
        QString("background-color: %1;").arg(emissive_colour_.name()));
    on_live_edit_changed();
  }
}

void SdfEditorWindow::on_pick_texture_clicked() {
  ScopedRenderPause pause(viewport_);
  QString path = QFileDialog::getOpenFileName(
      this, "Select Texture Image", QString(),
      "Images (*.png *.jpg *.jpeg *.bmp *.tga)");
  if (path.isEmpty()) {
    return;
  }

  QImage image(path);
  if (image.isNull()) {
    QMessageBox::warning(this, "Texture Load Failed",
                         "Could not read image: " + path);
    return;
  }

  QDir().mkpath("assets/textures");
  std::string base = sanitize_texture_name(
      QFileInfo(path).completeBaseName().toStdString());
  std::string dest = "assets/textures/" + base + ".png";
  // Re-saved through QImage regardless of the source format -- TextureSystem
  // (engine-side) only ever looks for "assets/textures/<name>.png" (see
  // texture_path() in texture_system.cpp), so a .jpg/.bmp/etc. source still
  // needs to land on disk as an actual .png.
  if (!image.save(QString::fromStdString(dest), "PNG")) {
    QMessageBox::warning(this, "Texture Copy Failed",
                         "Could not write " + QString::fromStdString(dest));
    return;
  }

  texture_name_ = base;
  texture_label_->setText(QString::fromStdString(texture_name_));
  on_live_edit_changed(); // apply immediately if a primitive is selected
}

void SdfEditorWindow::on_clear_texture_clicked() {
  if (texture_name_.empty()) {
    return;
  }
  texture_name_.clear();
  texture_label_->setText("(none)");
  on_live_edit_changed(); // apply immediately if a primitive is selected
}

void SdfEditorWindow::on_pick_bump_map_clicked() {
  ScopedRenderPause pause(viewport_);
  QString path = QFileDialog::getOpenFileName(
      this, "Select Bump Map Image", QString(),
      "Images (*.png *.jpg *.jpeg *.bmp *.tga)");
  if (path.isEmpty()) {
    return;
  }

  QImage image(path);
  if (image.isNull()) {
    QMessageBox::warning(this, "Bump Map Load Failed",
                         "Could not read image: " + path);
    return;
  }

  QDir().mkpath("assets/textures");
  std::string base = sanitize_texture_name(
      QFileInfo(path).completeBaseName().toStdString());
  std::string dest = "assets/textures/" + base + ".png";
  if (!image.save(QString::fromStdString(dest), "PNG")) {
    QMessageBox::warning(this, "Bump Map Copy Failed",
                         "Could not write " + QString::fromStdString(dest));
    return;
  }

  bump_map_name_ = base;
  bump_map_label_->setText(QString::fromStdString(bump_map_name_));
  on_live_edit_changed(); // apply immediately if a primitive is selected
}

void SdfEditorWindow::on_clear_bump_map_clicked() {
  if (bump_map_name_.empty()) {
    return;
  }
  bump_map_name_.clear();
  bump_map_label_->setText("(none)");
  on_live_edit_changed(); // apply immediately if a primitive is selected
}

void SdfEditorWindow::on_move_mode_clicked() {
  viewport_->set_gizmo_mode(GizmoMode::Translate);
}

void SdfEditorWindow::on_rotate_mode_clicked() {
  viewport_->set_gizmo_mode(GizmoMode::Rotate);
}

void SdfEditorWindow::on_splat_visibility_toggled(bool checked) {
  viewport_->set_splat_visibility(checked);
}

void SdfEditorWindow::on_show_grid_toggled(bool checked) {
  viewport_->set_grid_visible(checked);
}

void SdfEditorWindow::on_save_clicked() {
  ScopedRenderPause pause(viewport_);
  QString path =
      QFileDialog::getSaveFileName(this, "Save SDF Scene",
                                   "assets/scenes/authored_scene.sdf",
                                   "SDF Scene Files (*.sdf)");
  if (path.isEmpty()) {
    return;
  }
  if (!save_scene(path.toStdString(), scene_)) {
    QMessageBox::warning(this, "Save Failed",
                         "Could not write to " + path);
  }
}

void SdfEditorWindow::on_load_clicked() {
  ScopedRenderPause pause(viewport_);
  QString path = QFileDialog::getOpenFileName(
      this, "Load SDF Scene", "assets/scenes/", "SDF Scene Files (*.sdf)");
  if (path.isEmpty()) {
    return;
  }
  std::optional<SdfScene> loaded = read_scene(path.toStdString());
  if (!loaded) {
    QMessageBox::warning(this, "Load Failed", "Could not read " + path);
    return;
  }
  scene_ = std::move(*loaded);

  // Resume id generation above every "layerN"/"lightN" name already in this
  // file -- otherwise the next Add click could recompute an id already used
  // by a loaded layer/light, colliding with it (see next_layer_id_'s comment
  // in main_window.h). Assigned outright (not max()'d against the previous
  // file's counter): scene_ was just replaced wholesale, so only *this*
  // file's names can collide, and carrying the old high-water mark over
  // meant a freshly loaded file kept numbering new layers from wherever the
  // previous file left off.
  std::vector<std::string> layer_names;
  layer_names.reserve(scene_.layers.size());
  for (const SdfLayerDef &layer : scene_.layers) {
    layer_names.push_back(layer.name);
  }
  next_layer_id_ = next_id_after("layer", layer_names);

  std::vector<std::string> light_names;
  light_names.reserve(scene_.lights.size());
  for (const SdfLightDef &light : scene_.lights) {
    light_names.push_back(light.name);
  }
  next_light_id_ = next_id_after("light", light_names);

  std::vector<std::string> volumetric_names;
  volumetric_names.reserve(scene_.volumetrics.size());
  for (const SdfVolumetricDef &volumetric : scene_.volumetrics) {
    volumetric_names.push_back(volumetric.name);
  }
  next_volumetric_id_ = next_id_after("volumetric", volumetric_names);

  std::vector<std::string> primitive_names;
  for (const SdfLayerDef &layer : scene_.layers) {
    for (const SdfPrimitiveDef &primitive : layer.primitives) {
      primitive_names.push_back(primitive.name);
    }
  }
  next_primitive_id_ = next_id_after("primitive", primitive_names);

  refresh_contents_list();
  refresh_lights_list();
  refresh_volumetrics_list();
  refresh_material_list();
  {
    const QSignalBlocker blocker(ambient_spin_);
    ambient_spin_->setValue(scene_.ambient);
  }
  apply_scene_skybox();
  // A whole new scene's worth of geometry: re-arm chunk cache pre-warming
  // so it is baked into the cache up front, exactly as it would be for the
  // session's FIRST scene. Needed explicitly because sync_viewport_scene()
  // reconciles rather than loads for every scene after the first (see
  // sync_viewport_scene_now()), and only renderer_load_scene() re-arms by
  // itself -- so without this, pre-warming ran once per session and every
  // scene opened afterwards paid cold bakes as you flew around it.
  renderer_request_cache_prewarm();
  sync_viewport_scene();
  active_layer_index_ = -1;
  viewport_->set_selection({}); // previous selection is from a different
                                // scene entirely now
}

void SdfEditorWindow::sync_viewport_scene() {
  viewport_->set_scene(scene_); // keeps click-picking in sync too
  // A pending debounced sync (see request_viewport_resync()) is about to
  // be made redundant by the immediate one below -- stop it rather than
  // let it fire later and repeat the same (by then already-applied) work.
  sync_debounce_timer_->stop();
  sync_viewport_scene_now();
}

void SdfEditorWindow::request_viewport_resync() {
  viewport_->set_scene(scene_); // immediate, cheap -- keeps the gizmo and
                                // click-picking tracking every keystroke
                                // even while the real sync is deferred.
  sync_debounce_timer_->start(kSyncDebounceMs); // (re)starts if already
                                                // running, coalescing a
                                                // burst into one sync.
}

void SdfEditorWindow::sync_viewport_scene_now() {
  if (!save_scene(kLivePreviewPath, scene_)) {
    return; // save_scene() already logged why.
  }
  // First call ever (nothing registered yet) -- load fresh. Every call
  // after this reconciles against the same handle instead (see
  // live_scene_handle_'s own comment) so an edit that only touches one
  // primitive doesn't force the renderer to release and re-register this
  // editor's ENTIRE authored world -- which, for the chunked/streamed
  // field, meant re-baking every resident chunk regardless of how small
  // the actual edit was (see renderer_reconcile_scene()'s own comment).
  if (live_scene_handle_ == kInvalidSceneHandle) {
    live_scene_handle_ = renderer_load_scene(kLivePreviewPath).handle();
  } else {
    renderer_reconcile_scene(live_scene_handle_, kLivePreviewPath);
  }
}

std::string SdfEditorWindow::renderer_primitive_name(PrimitiveRef ref) const {
  if (live_scene_handle_ == kInvalidSceneHandle || ref.is_light() ||
      ref.layer_index < 0 ||
      ref.layer_index >= static_cast<int>(scene_.layers.size())) {
    return {};
  }
  const auto &layer = scene_.layers[ref.layer_index];
  if (ref.primitive_index < 0 ||
      ref.primitive_index >= static_cast<int>(layer.primitives.size())) {
    return {};
  }
  // Mirrors GeometrySystem::load_scene()'s own prefixing exactly; the two
  // must agree or the renderer will simply not find the primitive and the
  // drag falls back to the slow baked path.
  return "scene" + std::to_string(live_scene_handle_) + "/" + layer.name + "/" +
         layer.primitives[ref.primitive_index].name;
}

void SdfEditorWindow::on_gizmo_drag_started(PrimitiveRef primitive) {
  // An empty name (multi-selection, a light, or a ref that resolves to
  // nothing) leaves the renderer with no dynamic primitive, so that drag
  // simply behaves as it always did -- which the release path has to know
  // about, since its shortcut assumes there is a dynamic primitive to drop.
  // See drag_had_dynamic_primitive_.
  const std::string name = renderer_primitive_name(primitive);
  drag_had_dynamic_primitive_ = !name.empty();
  renderer_set_dynamic_primitive(name);
}

void SdfEditorWindow::on_gizmo_drag_moved(GizmoTransformResult transform) {
  // Three floats straight to the renderer. Deliberately NOT
  // sync_viewport_scene_now(): that serialises the ENTIRE scene to disk and
  // has the engine re-read and re-parse it, per mouse-move, to change one
  // primitive's position -- and it would send the wrong value anyway, since
  // it serialises this window's scene copy, which a drag does not touch
  // until it ends (the viewport mutates its own copy).
  //
  // Only for a drag that actually has a dynamic primitive. Live-pushing a
  // BAKED primitive's transform per mouse-move would dirty it per mouse-
  // move, i.e. re-bake the chunks it covers at frame rate -- and a
  // multi-selection drag is exactly the case with no dynamic primitive.
  // Those drags go back to what every drag did before dynamic primitives
  // existed: the viewport moves its own copy, and the single sync on
  // release commits the whole thing at once.
  if (!drag_had_dynamic_primitive_) {
    return;
  }
  const std::string name = renderer_primitive_name(transform.ref);
  if (name.empty()) {
    return;
  }
  // The viewport speaks in AUTHORED (layer-local) values -- they go
  // straight back onto the primitive on release. The renderer holds WORLD
  // ones (Geometry::position/rotation, with the layer's transform already
  // composed in -- see SdfLayerDef::position), so the conversion happens
  // here, at the one place a transform crosses from one to the other.
  //
  // Reading the layer out of THIS window's scene copy is safe even though
  // the viewport's copy is the one being dragged: a primitive drag never
  // touches its layer's transform, so the two copies cannot disagree
  // about it for the duration of the drag.
  SdfTransform world{transform.position, transform.rotation};
  if (transform.ref.layer_index >= 0 &&
      transform.ref.layer_index < static_cast<int>(scene_.layers.size()) &&
      transform.ref.primitive_index >= 0) {
    const SdfLayerDef &layer = scene_.layers[transform.ref.layer_index];
    const auto &primitives = layer.primitives;
    if (transform.ref.primitive_index < static_cast<int>(primitives.size())) {
      SdfPrimitiveDef dragged = primitives[transform.ref.primitive_index];
      dragged.position = transform.position;
      dragged.rotation = transform.rotation;
      world = sdf_layer_world_transform(layer, dragged);
    }
  }
  renderer_set_primitive_transform(name, world.position, world.rotation);
}

void SdfEditorWindow::on_gizmo_drag_ended() {
  // Hands it back to the bake. The commit that follows (on_viewport_
  // primitives_transformed -> sync) is what re-bakes the chunks its old and
  // new bounds cover, once, instead of on every mouse-move.
  renderer_set_dynamic_primitive({});
}

void SdfEditorWindow::on_viewport_selection_changed(std::vector<PrimitiveRef> selection) {
  // A viewport click never selects a light (pick_at() only ray-casts
  // against primitives -- see ray_intersect.h), so any light row still
  // showing selected in lights_list_ is now stale; clear it for
  // consistency with what the tree's about to show below.
  {
    const QSignalBlocker light_blocker(lights_list_);
    lights_list_->setCurrentItem(nullptr);
  }

  // Mirror viewport_'s selection onto contents_tree_ -- block its own
  // selection-changed signal while doing so, since
  // on_contents_tree_selection_changed() would otherwise just call
  // viewport_->set_selection() right back with what's already selected.
  const QSignalBlocker blocker(contents_tree_);
  contents_tree_->clearSelection();
  for (const PrimitiveRef &ref : selection) {
    for (int i = 0; i < contents_tree_->topLevelItemCount(); ++i) {
      QTreeWidgetItem *layer_item = contents_tree_->topLevelItem(i);
      if (layer_item->data(0, kLayerIndexRole).toInt() != ref.layer_index) {
        continue;
      }
      for (int j = 0; j < layer_item->childCount(); ++j) {
        QTreeWidgetItem *primitive_item = layer_item->child(j);
        if (primitive_item->data(0, kPrimitiveIndexRole).toInt() ==
            ref.primitive_index) {
          primitive_item->setSelected(true);
          contents_tree_->scrollToItem(primitive_item);
        }
      }
    }
  }

  // The signal blocker above means active_layer_index_/the side panel's
  // fields need updating by hand, exactly what
  // on_contents_tree_selection_changed() would otherwise have done.
  if (selection.size() == 1) {
    active_layer_index_ = selection.front().layer_index;
    populate_fields_from_selection(selection.front().layer_index,
                                   selection.front().primitive_index);
  } else if (selection.empty()) {
    active_layer_index_ = -1;
  } else {
    bool same_layer = std::all_of(
        selection.begin(), selection.end(), [&](const PrimitiveRef &ref) {
          return ref.layer_index == selection.front().layer_index;
        });
    active_layer_index_ = same_layer ? selection.front().layer_index : -1;
  }
}

void SdfEditorWindow::on_viewport_primitives_transformed(
    std::vector<GizmoTransformResult> results) {
  for (const GizmoTransformResult &result : results) {
    if (result.ref.is_light()) {
      if (result.ref.light_index >= 0 &&
          result.ref.light_index < static_cast<int>(scene_.lights.size())) {
        // Only position is meaningful for a light -- rotation/params are
        // always sent (see GizmoTransformResult's comment) but unused here.
        scene_.lights[result.ref.light_index].position = result.position;
      }
      continue;
    }
    if (result.ref.layer_index < 0 ||
        result.ref.layer_index >= static_cast<int>(scene_.layers.size())) {
      continue;
    }
    auto &primitives = scene_.layers[result.ref.layer_index].primitives;
    if (result.ref.primitive_index < 0 ||
        result.ref.primitive_index >= static_cast<int>(primitives.size())) {
      continue;
    }
    SdfPrimitiveDef &primitive = primitives[result.ref.primitive_index];
    primitive.position = result.position;
    primitive.rotation = result.rotation;
    primitive.params = result.params;
  }
  // Persist and re-push scene_ into viewport_, resolving the temporary
  // divergence between SdfEditorWindow's and SceneViewport's copies that
  // existed only during the drag itself.
  //
  // Skipping the reconcile is only sound for a drag that HAD a dynamic
  // primitive. There, the renderer is already correct: the drag pushed
  // every transform straight to it via renderer_set_primitive_transform(),
  // and dropping the dynamic primitive queued the chunks that need
  // re-baking. Reconciling as well would re-serialise the whole scene,
  // re-parse it, diff it, and trigger another full rebuild behind a
  // graphics-queue idle, to arrive at the state the renderer is already in
  // -- which is a large part of the stall felt on release.
  //
  // Without one -- a ctrl-click multi-selection, or a light -- nothing was
  // dropped, so nothing queued a re-bake, and renderer_set_primitive_
  // transform() deliberately doesn't mark the scene dirty either. The move
  // would reach the primitive buffer and never reach the baked field, and
  // the splat pass (which draws the image whenever no primitive is dynamic)
  // reads only the baked field: the primitives would not appear to move at
  // all until some later edit happened to dirty them. So fall back to the
  // ordinary sync, which is what every drag did before dynamic primitives
  // existed.
  //
  // The file has to be written either way -- the next ordinary edit
  // reconciles against it, and a stale one would silently revert this drag.
  if (!drag_had_dynamic_primitive_) {
    viewport_->set_scene(scene_);
    sync_debounce_timer_->stop(); // superseded by the sync below
    sync_viewport_scene_now();    // saves the file itself
  } else {
    if (!save_scene(kLivePreviewPath, scene_)) {
      return; // save_scene() already logged why
    }
    viewport_->set_scene(scene_);
    sync_debounce_timer_->stop(); // nothing pending is still relevant
  }
  // The drag just changed position/rotation/size without going through the
  // side panel's fields at all -- refresh them so they don't show stale
  // pre-drag values (only meaningful for a single-item selection -- see
  // populate_fields_from_selection()'s own comment).
  if (results.size() == 1) {
    const PrimitiveRef &ref = results.front().ref;
    if (ref.is_light()) {
      populate_light_fields_from_selection(ref.light_index);
    } else {
      populate_fields_from_selection(ref.layer_index, ref.primitive_index);
    }
  }
}

std::vector<PrimitiveRef> SdfEditorWindow::tree_selected_primitives() const {
  std::vector<PrimitiveRef> result;
  for (QTreeWidgetItem *item : contents_tree_->selectedItems()) {
    if (!item->parent()) {
      continue; // a layer row -- nothing to feed the gizmo/edit fields with
    }
    result.push_back(PrimitiveRef{item->data(0, kLayerIndexRole).toInt(),
                                  item->data(0, kPrimitiveIndexRole).toInt()});
  }
  return result;
}

void SdfEditorWindow::on_contents_tree_selection_changed() {
  // Selecting a primitive row here supersedes any light selection -- a
  // light and a primitive never show the gizmo together (see
  // on_lights_list_selection_changed()'s mirror of this).
  {
    const QSignalBlocker light_blocker(lights_list_);
    lights_list_->setCurrentItem(nullptr);
  }

  std::vector<PrimitiveRef> selection = tree_selected_primitives();
  viewport_->set_selection(selection);

  if (selection.size() == 1) {
    active_layer_index_ = selection.front().layer_index;
    properties_stack_->setCurrentIndex(kPrimitivePropertiesPage);
    populate_fields_from_selection(selection.front().layer_index,
                                   selection.front().primitive_index);
    return;
  }
  if (!selection.empty()) {
    properties_stack_->setCurrentIndex(kPrimitivePropertiesPage);
    bool same_layer = std::all_of(
        selection.begin(), selection.end(), [&](const PrimitiveRef &ref) {
          return ref.layer_index == selection.front().layer_index;
        });
    active_layer_index_ = same_layer ? selection.front().layer_index : -1;
    return;
  }

  // Nothing (primitive-wise) selected -- a lone layer row still counts as
  // "active" for on_add_clicked(), so Add Primitive can target it.
  QList<QTreeWidgetItem *> selected_items = contents_tree_->selectedItems();
  if (selected_items.size() == 1 && !selected_items.front()->parent()) {
    active_layer_index_ = selected_items.front()->data(0, kLayerIndexRole).toInt();
    // Exactly one layer row and nothing else: show that layer's own
    // properties instead of the primitive fields (see properties_stack_).
    properties_stack_->setCurrentIndex(kLayerPropertiesPage);
    populate_layer_fields(active_layer_index_);
  } else {
    active_layer_index_ = -1;
    properties_stack_->setCurrentIndex(kPrimitivePropertiesPage);
  }
}

void SdfEditorWindow::populate_layer_fields(int layer_index) {
  if (layer_index < 0 || layer_index >= static_cast<int>(scene_.layers.size())) {
    return;
  }
  const SdfLayerDef &layer = scene_.layers[layer_index];

  // Same guard as populating_fields_ on the primitive page -- setValue()
  // here would otherwise "edit" the layer straight back (see its comment).
  populating_layer_fields_ = true;
  layer_operation_combo_->setCurrentIndex(
      layer.operation == SdfLayerOperation::Subtraction ? 1 : 0);
  layer_smoothness_spin_->setValue(layer.smoothness);
  layer_pos_x_->setValue(layer.position.x);
  layer_pos_y_->setValue(layer.position.y);
  layer_pos_z_->setValue(layer.position.z);
  // Stored in radians, shown in degrees -- see populate_fields_from_
  // selection()'s identical conversion for a primitive's own rotation.
  const glm::vec3 layer_rotation_degrees = glm::degrees(layer.rotation);
  layer_rot_x_->setValue(layer_rotation_degrees.x);
  layer_rot_y_->setValue(layer_rotation_degrees.y);
  layer_rot_z_->setValue(layer_rotation_degrees.z);
  layer_repetition_combo_->setCurrentIndex(
      static_cast<int>(layer.repetition_mode));
  layer_repeat_cell_x_->setValue(layer.repetition_cell.x);
  layer_repeat_cell_y_->setValue(layer.repetition_cell.y);
  layer_repeat_cell_z_->setValue(layer.repetition_cell.z);
  layer_repeat_count_x_->setValue(layer.repetition_count.x);
  layer_repeat_count_y_->setValue(layer.repetition_count.y);
  layer_repeat_count_z_->setValue(layer.repetition_count.z);
  populating_layer_fields_ = false;

  update_layer_field_enablement();
}

void SdfEditorWindow::update_layer_field_enablement() {
  // Which cell/count components a mode actually reads -- exactly the rules
  // the primitive page's own repetition fields follow (see
  // update_field_enablement()), applied to the layer's copies.
  auto mode = static_cast<SdfRepetitionMode>(
      layer_repetition_combo_->currentIndex());
  bool linear = mode == SdfRepetitionMode::Infinite ||
                mode == SdfRepetitionMode::Limited;
  bool rectangular = mode == SdfRepetitionMode::Rectangular;
  bool rotational = mode == SdfRepetitionMode::Rotational;

  layer_repeat_cell_x_->setEnabled(linear || rectangular);
  layer_repeat_cell_y_->setEnabled(linear);
  layer_repeat_cell_z_->setEnabled(linear || rectangular);
  // Infinite never stops, so it has no count at all; Rotational's count is
  // a single copy count, in X.
  layer_repeat_count_x_->setEnabled(mode == SdfRepetitionMode::Limited ||
                                    rectangular || rotational);
  layer_repeat_count_y_->setEnabled(mode == SdfRepetitionMode::Limited);
  layer_repeat_count_z_->setEnabled(mode == SdfRepetitionMode::Limited ||
                                    rectangular);
}

void SdfEditorWindow::on_layer_repetition_mode_changed() {
  update_layer_field_enablement(); // re-grey the cell/count fields for the
                                   // newly chosen mode
  on_layer_field_changed();
}

void SdfEditorWindow::on_layer_field_changed() {
  if (populating_layer_fields_ || !contents_tree_) {
    return; // see on_live_edit_changed()'s identical guard
  }
  if (active_layer_index_ < 0 ||
      active_layer_index_ >= static_cast<int>(scene_.layers.size())) {
    return;
  }
  // Only when a LAYER row is what's selected -- active_layer_index_ is also
  // set for a primitive selection (it's where Add Primitive would go), and
  // these fields say nothing about the layer then.
  if (properties_stack_->currentIndex() != kLayerPropertiesPage) {
    return;
  }

  SdfLayerDef &layer = scene_.layers[active_layer_index_];
  layer.operation = layer_operation_combo_->currentIndex() == 1
                        ? SdfLayerOperation::Subtraction
                        : SdfLayerOperation::Union;
  layer.smoothness = static_cast<f32>(layer_smoothness_spin_->value());
  layer.position = glm::vec3(static_cast<f32>(layer_pos_x_->value()),
                             static_cast<f32>(layer_pos_y_->value()),
                             static_cast<f32>(layer_pos_z_->value()));
  layer.rotation = glm::radians(
      glm::vec3(static_cast<f32>(layer_rot_x_->value()),
                static_cast<f32>(layer_rot_y_->value()),
                static_cast<f32>(layer_rot_z_->value())));
  layer.repetition_mode =
      static_cast<SdfRepetitionMode>(layer_repetition_combo_->currentIndex());
  layer.repetition_cell =
      glm::vec3(static_cast<f32>(layer_repeat_cell_x_->value()),
                static_cast<f32>(layer_repeat_cell_y_->value()),
                static_cast<f32>(layer_repeat_cell_z_->value()));
  layer.repetition_count =
      glm::vec3(static_cast<f32>(layer_repeat_count_x_->value()),
                static_cast<f32>(layer_repeat_count_y_->value()),
                static_cast<f32>(layer_repeat_count_z_->value()));

  // Relabel the row in place rather than calling refresh_contents_list():
  // rebuilding the tree clears its selection, which would drop this page
  // (and the field being edited) out from under the user mid-edit.
  for (int i = 0; i < contents_tree_->topLevelItemCount(); ++i) {
    QTreeWidgetItem *item = contents_tree_->topLevelItem(i);
    if (item->data(0, kLayerIndexRole).toInt() == active_layer_index_) {
      item->setText(0, layer_item_text(layer));
      break;
    }
  }

  request_viewport_resync();
}

void SdfEditorWindow::on_primitives_reparented() { sync_layers_from_tree(); }

void SdfEditorWindow::sync_layers_from_tree() {
  // Steal every primitive out of scene_.layers, keyed by its stable name --
  // a drag-and-drop reparent only ever changes which layer a primitive
  // item sits under in the tree, never its own name, so this is the only
  // safe way to re-find each one once (layer_index, primitive_index) pairs
  // are exactly what the drag just invalidated.
  std::unordered_map<std::string, SdfPrimitiveDef> primitives_by_name;
  for (SdfLayerDef &layer : scene_.layers) {
    for (SdfPrimitiveDef &primitive : layer.primitives) {
      primitives_by_name.emplace(primitive.name, std::move(primitive));
    }
  }

  std::vector<SdfLayerDef> new_layers;
  new_layers.reserve(contents_tree_->topLevelItemCount());
  for (int i = 0; i < contents_tree_->topLevelItemCount(); ++i) {
    QTreeWidgetItem *layer_item = contents_tree_->topLevelItem(i);
    int old_layer_index = layer_item->data(0, kLayerIndexRole).toInt();
    if (old_layer_index < 0 || old_layer_index >= static_cast<int>(scene_.layers.size())) {
      continue; // shouldn't happen -- refresh_contents_list() always
                // stamps a valid index
    }
    // A primitive drag changes NOTHING about a layer except which
    // primitives it holds, so the layer is copied whole and only its
    // primitives[] rebuilt. Deliberately a copy rather than a field-by-
    // field rebuild: listing the fields to carry over means every property
    // a layer gains afterwards is silently reset by any tree drag until
    // someone remembers to add a line here -- which is exactly what
    // happened to the layer's repetition (mode/cell/count), wiped
    // scene-wide by a single drag-and-drop reparent.
    SdfLayerDef new_layer = scene_.layers[old_layer_index];
    // The primitives were moved out into primitives_by_name above, so what
    // the copy carries are husks; the real ones are pushed back below.
    new_layer.primitives.clear();

    for (int j = 0; j < layer_item->childCount(); ++j) {
      QTreeWidgetItem *primitive_item = layer_item->child(j);
      std::string name =
          primitive_item->data(0, kPrimitiveNameRole).toString().toStdString();
      auto it = primitives_by_name.find(name);
      if (it != primitives_by_name.end()) {
        new_layer.primitives.push_back(std::move(it->second));
      }
    }
    new_layers.push_back(std::move(new_layer));
  }
  scene_.layers = std::move(new_layers);

  refresh_contents_list();
  sync_viewport_scene();
  // Every (layer_index, primitive_index) pair the drag touched is stale --
  // simplest to just drop the selection rather than try to track where
  // each dragged item landed.
  viewport_->set_selection({});
  active_layer_index_ = -1;
}

void SdfEditorWindow::populate_fields_from_selection(int layer_index,
                                                     int primitive_index) {
  if (layer_index < 0 || layer_index >= static_cast<int>(scene_.layers.size())) {
    return;
  }
  const SdfLayerDef &layer = scene_.layers[layer_index];
  if (primitive_index < 0 || primitive_index >= static_cast<int>(layer.primitives.size())) {
    return;
  }
  const SdfPrimitiveDef &primitive = layer.primitives[primitive_index];
  PrimitiveTypeSpec spec = type_spec_for(primitive.type);

  populating_fields_ = true;

  // SdfPrimitiveType and the type list are both ordered identically -- see
  // the constructor's type_list_ population loop.
  type_list_->setCurrentRow(static_cast<int>(primitive.type));

  operation_combo_->setCurrentIndex(
      layer.operation == SdfLayerOperation::Subtraction ? 1 : 0);
  smoothness_spin_->setValue(layer.smoothness);

  pos_x_->setValue(primitive.position.x);
  pos_y_->setValue(primitive.position.y);
  pos_z_->setValue(primitive.position.z);

  glm::vec3 rotation_degrees = glm::degrees(primitive.rotation);
  rot_x_->setValue(rotation_degrees.x);
  rot_y_->setValue(rotation_degrees.y);
  rot_z_->setValue(rotation_degrees.z);

  f32 raw_params[4] = {primitive.params.x, primitive.params.y, primitive.params.z,
                       primitive.extra_param};
  for (size_t i = 0; i < spec.param_labels.size() && i < 4; ++i) {
    param_spin_[i]->setValue(raw_params[i]);
    param_expr_edit_[i]->setText(QString::fromStdString(primitive.param_expressions[i]));
  }

  repetition_combo_->setCurrentIndex(static_cast<int>(primitive.repetition_mode));
  repeat_cell_x_->setValue(primitive.repetition_cell.x);
  repeat_cell_y_->setValue(primitive.repetition_cell.y);
  repeat_cell_z_->setValue(primitive.repetition_cell.z);
  repeat_count_x_->setValue(primitive.repetition_count.x);
  repeat_count_y_->setValue(primitive.repetition_count.y);
  repeat_count_z_->setValue(primitive.repetition_count.z);

  twist_spin_->setValue(primitive.twist);
  bend_spin_->setValue(primitive.bend);
  bend_axis_combo_->setCurrentIndex(static_cast<int>(primitive.bend_axis));
  displace_amplitude_spin_->setValue(primitive.displace_amplitude);
  displace_frequency_spin_->setValue(primitive.displace_frequency);

  // Resolved through the scene's material library, with this primitive's
  // own overrides folded in -- the same function the renderer resolves
  // through, so the panel cannot disagree with what is on screen.
  populate_fields_from_material(sdf_scene_resolve_material(scene_, primitive));

  populating_fields_ = false;

  update_field_enablement();
}

void SdfEditorWindow::apply_fields_to_primitive(int layer_index, int primitive_index) {
  SdfLayerDef &layer = scene_.layers[layer_index];
  if (primitive_index < 0 || primitive_index >= static_cast<int>(layer.primitives.size())) {
    return;
  }
  SdfPrimitiveDef &primitive = layer.primitives[primitive_index];
  PrimitiveTypeSpec spec = type_spec_for(primitive.type);

  layer.operation = operation_combo_->currentIndex() == 1
                        ? SdfLayerOperation::Subtraction
                        : SdfLayerOperation::Union;
  layer.smoothness = static_cast<f32>(smoothness_spin_->value());

  if (spec.has_position) {
    primitive.position = glm::vec3(static_cast<f32>(pos_x_->value()),
                                  static_cast<f32>(pos_y_->value()),
                                  static_cast<f32>(pos_z_->value()));
  }
  if (spec.has_rotation) {
    primitive.rotation = glm::radians(
        glm::vec3(static_cast<f32>(rot_x_->value()), static_cast<f32>(rot_y_->value()),
                 static_cast<f32>(rot_z_->value())));
  }

  f32 raw_params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (size_t i = 0; i < spec.param_labels.size() && i < 4; ++i) {
    raw_params[i] = static_cast<f32>(param_spin_[i]->value());
    primitive.param_expressions[i] = param_expr_edit_[i]->text().toStdString();
  }
  primitive.params = glm::vec3(raw_params[0], raw_params[1], raw_params[2]);
  primitive.extra_param = raw_params[3];

  primitive.repetition_mode =
      static_cast<SdfRepetitionMode>(repetition_combo_->currentIndex());
  primitive.repetition_cell =
      glm::vec3(static_cast<f32>(repeat_cell_x_->value()),
               static_cast<f32>(repeat_cell_y_->value()),
               static_cast<f32>(repeat_cell_z_->value()));
  primitive.repetition_count =
      glm::vec3(static_cast<f32>(repeat_count_x_->value()),
               static_cast<f32>(repeat_count_y_->value()),
               static_cast<f32>(repeat_count_z_->value()));

  primitive.twist = static_cast<f32>(twist_spin_->value());
  primitive.bend = static_cast<f32>(bend_spin_->value());
  primitive.bend_axis =
      static_cast<SdfBendAxis>(bend_axis_combo_->currentIndex());
  primitive.displace_amplitude = static_cast<f32>(displace_amplitude_spin_->value());
  primitive.displace_frequency = static_cast<f32>(displace_frequency_spin_->value());

  // Edits the bound material in place when this primitive is its only
  // user, and forks only when it is shared -- see
  // ensure_material_binding().
  primitive.material_id = ensure_material_binding(primitive.material_id);
  primitive.material_name.clear();
}

void SdfEditorWindow::on_live_edit_changed() {
  if (populating_fields_ || !contents_tree_) {
    // !contents_tree_: a field's valueChanged can fire from the
    // constructor itself (setting an initial default after the signal is
    // already connected), before contents_tree_ exists yet.
    return;
  }
  // The property widgets are shared between "edit this primitive's
  // material" and "edit this material", and the open tab is what says
  // which. With the Materials tab open the edit lands on the selected
  // library material itself, so every primitive referencing it follows --
  // the thing the old filename scheme made impossible at any price.
  if (tabs_ && materials_tab_index_ >= 0 &&
      tabs_->currentIndex() == materials_tab_index_) {
    apply_fields_to_selected_material();
    return;
  }
  // Only meaningful for exactly one selected primitive -- these fields
  // can't sensibly live-edit several primitives at once if they're
  // different types/materials (see on_contents_tree_selection_changed()'s
  // own comment).
  std::vector<PrimitiveRef> selection = tree_selected_primitives();
  if (selection.size() != 1) {
    return; // nothing (or more than one thing) selected -- fields are just
           // staging values for Add
  }
  int layer_index = selection.front().layer_index;
  if (layer_index < 0 || layer_index >= static_cast<int>(scene_.layers.size())) {
    return;
  }
  apply_fields_to_primitive(layer_index, selection.front().primitive_index);
  request_viewport_resync();
}

void SdfEditorWindow::on_param_expr_changed() {
  update_field_enablement(); // re-grey param_spin_[i] to match which
                            // param_expr_edit_[i] now have text
  on_live_edit_changed();
}

void SdfEditorWindow::on_repetition_mode_changed() {
  update_field_enablement(); // re-grey repeat_cell_*_/repeat_count_*_ to
                            // match the newly chosen mode
  on_live_edit_changed();
}

void SdfEditorWindow::on_light_type_changed() {
  bool is_point = light_type_combo_->currentIndex() == 1;
  light_vector_label_->setText(is_point ? "Position (x, y, z):"
                                       : "Direction (x, y, z):");
}

void SdfEditorWindow::on_add_light_clicked() {
  std::string name = "light" + std::to_string(next_light_id_++);
  glm::vec3 vec(static_cast<f32>(light_vec_x_->value()),
               static_cast<f32>(light_vec_y_->value()),
               static_cast<f32>(light_vec_z_->value()));
  glm::vec3 colour(static_cast<f32>(light_colour_.redF()),
                   static_cast<f32>(light_colour_.greenF()),
                   static_cast<f32>(light_colour_.blueF()));
  f32 intensity = static_cast<f32>(light_intensity_spin_->value());

  if (light_type_combo_->currentIndex() == 1) {
    add_point_light(scene_, name, vec, colour, intensity);
  } else {
    add_directional_light(scene_, name, vec, colour, intensity);
  }

  refresh_lights_list();
  sync_viewport_scene();
}

void SdfEditorWindow::on_remove_light_clicked() {
  QListWidgetItem *item = lights_list_->currentItem();
  if (!item) {
    return;
  }
  int light_index = item->data(Qt::UserRole).toInt();
  if (light_index >= 0 && light_index < static_cast<int>(scene_.lights.size())) {
    scene_.lights.erase(scene_.lights.begin() + light_index);
  }
  refresh_lights_list();
  sync_viewport_scene();
  viewport_->set_selection({}); // the just-removed light can't stay selected
}

void SdfEditorWindow::on_pick_light_colour_clicked() {
  ScopedRenderPause pause(viewport_);
  QColor picked = QColorDialog::getColor(light_colour_, this,
                                        "Select Light Colour");
  if (picked.isValid()) {
    light_colour_ = picked;
    light_colour_button_->setStyleSheet(
        QString("background-color: %1;").arg(light_colour_.name()));
    on_light_field_changed(); // apply immediately if a light is selected
  }
}

void SdfEditorWindow::on_lights_list_selection_changed() {
  QListWidgetItem *item = lights_list_->currentItem();
  int light_index = item ? item->data(Qt::UserRole).toInt() : -1;
  if (light_index >= 0) {
    populate_light_fields_from_selection(light_index);
  }

  // Selecting a light here supersedes any primitive selection -- mirrors
  // on_contents_tree_selection_changed()'s own clearing of lights_list_.
  {
    const QSignalBlocker tree_blocker(contents_tree_);
    contents_tree_->clearSelection();
  }
  active_layer_index_ = -1;

  update_viewport_light_selection(light_index);
}

void SdfEditorWindow::populate_light_fields_from_selection(int light_index) {
  if (light_index < 0 || light_index >= static_cast<int>(scene_.lights.size())) {
    return;
  }
  const SdfLightDef &light = scene_.lights[light_index];

  populating_light_fields_ = true;

  light_type_combo_->setCurrentIndex(light.type == SdfLightType::Point ? 1 : 0);
  glm::vec3 vec = light.type == SdfLightType::Point ? light.position
                                                    : light.direction;
  light_vec_x_->setValue(vec.x);
  light_vec_y_->setValue(vec.y);
  light_vec_z_->setValue(vec.z);

  light_colour_ = QColor::fromRgbF(light.colour.r, light.colour.g, light.colour.b);
  light_colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(light_colour_.name()));
  light_intensity_spin_->setValue(light.intensity);

  populating_light_fields_ = false;

  on_light_type_changed(); // relabel light_vector_label_ to match
}

void SdfEditorWindow::apply_fields_to_light(int light_index) {
  SdfLightDef &light = scene_.lights[light_index];

  light.type = light_type_combo_->currentIndex() == 1 ? SdfLightType::Point
                                                      : SdfLightType::Directional;
  glm::vec3 vec(static_cast<f32>(light_vec_x_->value()),
               static_cast<f32>(light_vec_y_->value()),
               static_cast<f32>(light_vec_z_->value()));
  if (light.type == SdfLightType::Point) {
    light.position = vec;
  } else {
    light.direction = vec;
  }
  light.colour = glm::vec3(static_cast<f32>(light_colour_.redF()),
                          static_cast<f32>(light_colour_.greenF()),
                          static_cast<f32>(light_colour_.blueF()));
  light.intensity = static_cast<f32>(light_intensity_spin_->value());
}

void SdfEditorWindow::update_viewport_light_selection(int light_index) {
  if (light_index >= 0 && light_index < static_cast<int>(scene_.lights.size()) &&
      scene_.lights[light_index].type == SdfLightType::Point) {
    viewport_->set_selection({PrimitiveRef{-1, -1, light_index}});
  } else {
    viewport_->set_selection({});
  }
}

void SdfEditorWindow::on_light_field_changed() {
  if (populating_light_fields_ || !lights_list_) {
    // !lights_list_: a field's valueChanged can fire from the constructor
    // itself (setting an initial default after the signal is already
    // connected), before lights_list_ exists yet.
    return;
  }
  QListWidgetItem *item = lights_list_->currentItem();
  if (!item) {
    return; // nothing selected -- fields are just staging values for Add
  }
  int light_index = item->data(Qt::UserRole).toInt();
  if (light_index < 0 || light_index >= static_cast<int>(scene_.lights.size())) {
    return;
  }
  apply_fields_to_light(light_index);
  request_viewport_resync();
  // Keeps the gizmo in sync with a Type toggle -- e.g. switching from Point
  // to Directional should hide it (a directional light has no position for
  // the gizmo to show), and the reverse should show it again. A no-op
  // (recomputes the same selection) for any other field's change.
  update_viewport_light_selection(light_index);
}

void SdfEditorWindow::on_ambient_changed() {
  scene_.ambient = static_cast<f32>(ambient_spin_->value());
  request_viewport_resync();
}

void SdfEditorWindow::on_pick_skybox_clicked() {
  ScopedRenderPause pause(viewport_);
  QString path = QFileDialog::getOpenFileName(
      this, "Select Skybox Image", "assets/textures/",
      "Images (*.png *.jpg *.jpeg *.bmp *.tga)");
  if (path.isEmpty()) {
    return;
  }

  QImage image(path);
  if (image.isNull()) {
    QMessageBox::warning(this, "Skybox Load Failed",
                         "Could not read image: " + path);
    return;
  }

  QDir().mkpath("assets/textures");
  std::string base = sanitize_texture_name(
      QFileInfo(path).completeBaseName().toStdString());
  std::string dest = "assets/textures/" + base + ".png";
  // Imported through QImage exactly the way a primitive's diffuse map is
  // (see on_pick_texture_clicked()) -- TextureSystem only ever opens
  // "assets/textures/<name>.png", so a source in any other format, or from
  // anywhere else on disk, has to land there as an actual .png first.
  //
  // Re-picking an image already in assets/textures/ rewrites it with a
  // pixel-identical copy of itself, which is harmless.
  if (!image.save(QString::fromStdString(dest), "PNG")) {
    QMessageBox::warning(this, "Skybox Copy Failed",
                         "Could not write " + QString::fromStdString(dest));
    return;
  }

  scene_.skybox = base;
  skybox_label_->setText(QString::fromStdString(scene_.skybox));
  // Written into the scene and pushed by the ordinary sync, which reaches
  // the renderer through VulkanRendererBackend::reconcile_scene()'s own
  // SdfScene::skybox handling -- no direct renderer call needed for this
  // direction. Immediate rather than debounced: choosing a backdrop is a
  // deliberate, one-off act, not a spinbox being dragged.
  sync_viewport_scene();
}

void SdfEditorWindow::on_clear_skybox_clicked() {
  if (scene_.skybox.empty()) {
    return;
  }
  scene_.skybox.clear();
  skybox_label_->setText("(none)");
  // The one direction the scene file cannot express. An empty
  // SdfScene::skybox means "unspecified" and is deliberately ignored on
  // load (see its comment), so a sync alone would leave the old sky on
  // screen until the next restart -- the renderer has to be told directly.
  renderer_disable_sky_box();
  sync_viewport_scene();
}

void SdfEditorWindow::apply_scene_skybox() {
  skybox_label_->setText(scene_.skybox.empty()
                             ? QString("(none)")
                             : QString::fromStdString(scene_.skybox));
  if (scene_.skybox.empty()) {
    // Same asymmetry on_clear_skybox_clicked() explains: opening a scene
    // that names no skybox must actually clear whatever the previously
    // open scene left on screen, and only a direct call can say that.
    renderer_disable_sky_box();
  } else {
    renderer_enable_sky_box(scene_.skybox);
  }
}

void SdfEditorWindow::refresh_lights_list() {
  lights_list_->clear();
  for (int i = 0; i < static_cast<int>(scene_.lights.size()); ++i) {
    const SdfLightDef &light = scene_.lights[i];
    const char *type_label =
        light.type == SdfLightType::Point ? "Point" : "Directional";
    QString text = QString("%1 '%2'")
                       .arg(type_label)
                       .arg(QString::fromStdString(light.name));
    auto *item = new QListWidgetItem(text, lights_list_);
    item->setData(Qt::UserRole, i);
  }
}

QString SdfEditorWindow::layer_item_text(const SdfLayerDef &layer) {
  const char *op_label =
      layer.operation == SdfLayerOperation::Subtraction ? "Subtract" : "Union";
  QString text = QString("%1 [%2, smoothness %3]")
                     .arg(QString::fromStdString(layer.name))
                     .arg(op_label)
                     .arg(layer.smoothness, 0, 'f', 2);
  // A repeated layer looks identical to an unrepeated one in this tree
  // otherwise -- it holds exactly the same primitives -- so the mode (and,
  // where it has one, the copy count) is worth the few characters.
  if (layer.repetition_mode != SdfRepetitionMode::None) {
    switch (layer.repetition_mode) {
    case SdfRepetitionMode::Infinite:
      text += " (repeat: infinite)";
      break;
    case SdfRepetitionMode::Limited:
      text += QString(" (repeat: %1x%2x%3)")
                  .arg(layer.repetition_count.x, 0, 'f', 0)
                  .arg(layer.repetition_count.y, 0, 'f', 0)
                  .arg(layer.repetition_count.z, 0, 'f', 0);
      break;
    case SdfRepetitionMode::Rotational:
      text += QString(" (repeat: %1 around Y)")
                  .arg(layer.repetition_count.x, 0, 'f', 0);
      break;
    case SdfRepetitionMode::Rectangular:
      text += QString(" (repeat: %1x%2 on XZ)")
                  .arg(layer.repetition_count.x, 0, 'f', 0)
                  .arg(layer.repetition_count.z, 0, 'f', 0);
      break;
    case SdfRepetitionMode::None:
      break;
    }
  }
  // Same reasoning as the repetition marker above, and a bit more urgent:
  // a transformed layer is why its primitives' Position fields no longer
  // read as world coordinates (see SdfLayerDef::position). Without a
  // marker here that is invisible unless the layer row happens to be the
  // one selected.
  if (layer.position != glm::vec3(0.0f)) {
    text += QString(" (moved %1, %2, %3)")
                .arg(layer.position.x, 0, 'f', 2)
                .arg(layer.position.y, 0, 'f', 2)
                .arg(layer.position.z, 0, 'f', 2);
  }
  if (layer.rotation != glm::vec3(0.0f)) {
    const glm::vec3 degrees = glm::degrees(layer.rotation);
    text += QString(" (turned %1°, %2°, %3°)")
                .arg(degrees.x, 0, 'f', 0)
                .arg(degrees.y, 0, 'f', 0)
                .arg(degrees.z, 0, 'f', 0);
  }
  return text;
}

void SdfEditorWindow::refresh_contents_list() {
  contents_tree_->clear();
  for (int i = 0; i < static_cast<int>(scene_.layers.size()); ++i) {
    const SdfLayerDef &layer = scene_.layers[i];
    QString layer_text = layer_item_text(layer);
    auto *layer_item = new QTreeWidgetItem(contents_tree_, {layer_text});
    layer_item->setData(0, kLayerIndexRole, i);
    layer_item->setExpanded(true);

    for (int j = 0; j < static_cast<int>(layer.primitives.size()); ++j) {
      const SdfPrimitiveDef &primitive = layer.primitives[j];
      QString text = QString("%1 '%2'")
                         .arg(primitive_type_label(primitive.type))
                         .arg(QString::fromStdString(primitive.name));
      auto *primitive_item = new QTreeWidgetItem(layer_item, {text});
      primitive_item->setData(0, kLayerIndexRole, i);
      primitive_item->setData(0, kPrimitiveIndexRole, j);
      primitive_item->setData(0, kPrimitiveNameRole,
                              QString::fromStdString(primitive.name));
    }
  }
}

void SdfEditorWindow::on_volumetric_type_changed() {
  update_volumetric_field_enablement();
}

void SdfEditorWindow::update_volumetric_field_enablement() {
  int row = volumetric_type_list_->currentRow();
  if (row < 0) {
    return;
  }
  PrimitiveTypeSpec spec = type_spec_for(static_cast<SdfPrimitiveType>(row));

  volumetric_pos_x_->setEnabled(spec.has_position);
  volumetric_pos_y_->setEnabled(spec.has_position);
  volumetric_pos_z_->setEnabled(spec.has_position);
  volumetric_rot_x_->setEnabled(spec.has_rotation);
  volumetric_rot_y_->setEnabled(spec.has_rotation);
  volumetric_rot_z_->setEnabled(spec.has_rotation);

  for (int i = 0; i < 4; ++i) {
    bool used = static_cast<size_t>(i) < spec.param_labels.size();
    volumetric_param_label_[i]->setVisible(used);
    volumetric_param_spin_[i]->setVisible(used);
    if (used) {
      volumetric_param_label_[i]->setText(
          QString::fromLatin1(spec.param_labels[i]) + ":");
    }
  }
}

MaterialDef SdfEditorWindow::volumetric_material_def_from_fields() const {
  // A volumetric's material is an ordinary MaterialDef whose emissive,
  // bump and flag properties simply sit at their defaults -- which is why
  // this no longer needs the separate serialisation path the old
  // filename encoder required.
  MaterialDef def;
  def.base_colour = glm::vec4(static_cast<f32>(volumetric_colour_.redF()),
                             static_cast<f32>(volumetric_colour_.greenF()),
                             static_cast<f32>(volumetric_colour_.blueF()),
                             static_cast<f32>(volumetric_colour_.alphaF()));
  def.base_map = volumetric_texture_name_;
  def.uv_scale = static_cast<f32>(volumetric_texture_scale_spin_->value());
  def.uv_offset =
      glm::vec3(static_cast<f32>(volumetric_texture_offset_x_->value()),
               static_cast<f32>(volumetric_texture_offset_y_->value()),
               static_cast<f32>(volumetric_texture_offset_z_->value()));
  def.uv_rotation = glm::radians(
      static_cast<f32>(volumetric_texture_rotation_spin_->value()));
  return def;
}

void SdfEditorWindow::populate_volumetric_fields_from_material(
    const MaterialDef &def) {
  volumetric_colour_ = QColor::fromRgbF(def.base_colour.r, def.base_colour.g,
                                        def.base_colour.b, def.base_colour.a);
  volumetric_colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(volumetric_colour_.name()));
  volumetric_texture_name_ = def.base_map;
  volumetric_texture_label_->setText(
      volumetric_texture_name_.empty()
          ? QStringLiteral("(none)")
          : QString::fromStdString(volumetric_texture_name_));
  volumetric_texture_scale_spin_->setValue(def.uv_scale);
  volumetric_texture_offset_x_->setValue(def.uv_offset.x);
  volumetric_texture_offset_y_->setValue(def.uv_offset.y);
  volumetric_texture_offset_z_->setValue(def.uv_offset.z);
  volumetric_texture_rotation_spin_->setValue(glm::degrees(def.uv_rotation));
}

MaterialId SdfEditorWindow::ensure_volumetric_material_binding(
    MaterialId existing_id) {
  const MaterialDef fields = volumetric_material_def_from_fields();
  if (MaterialDef *existing = sdf_scene_find_material(scene_, existing_id)) {
    if (material_use_count(existing_id) <= 1) {
      MaterialDef updated = fields;
      updated.id = existing->id;
      updated.display_name = existing->display_name;
      updated.unknown_keys = existing->unknown_keys;
      *existing = std::move(updated);
      refresh_material_list();
      return updated.id;
    }
  }
  return find_or_create_material(fields);
}

void SdfEditorWindow::on_add_volumetric_clicked() {
  int row = volumetric_type_list_->currentRow();
  if (row < 0) {
    return;
  }
  SdfPrimitiveType type = static_cast<SdfPrimitiveType>(row);
  PrimitiveTypeSpec spec = type_spec_for(type);

  const MaterialId material_id =
      find_or_create_material(volumetric_material_def_from_fields());
  std::string name = "volumetric" + std::to_string(next_volumetric_id_++);

  glm::vec3 position =
      spec.has_position
          ? glm::vec3(static_cast<f32>(volumetric_pos_x_->value()),
                     static_cast<f32>(volumetric_pos_y_->value()),
                     static_cast<f32>(volumetric_pos_z_->value()))
          : glm::vec3(0.0f);
  glm::vec3 rotation =
      spec.has_rotation
          ? glm::radians(glm::vec3(static_cast<f32>(volumetric_rot_x_->value()),
                                  static_cast<f32>(volumetric_rot_y_->value()),
                                  static_cast<f32>(volumetric_rot_z_->value())))
          : glm::vec3(0.0f);

  f32 raw_params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (size_t i = 0; i < spec.param_labels.size() && i < 4; ++i) {
    raw_params[i] = static_cast<f32>(volumetric_param_spin_[i]->value());
  }
  glm::vec3 params(raw_params[0], raw_params[1], raw_params[2]);
  f32 extra_param = raw_params[3];
  f32 density = static_cast<f32>(volumetric_density_spin_->value());

  add_volumetric(scene_, name, type, position, rotation, params, extra_param,
                 density, std::string{})
      .material_id = material_id;

  refresh_volumetrics_list();
  sync_viewport_scene();
}

void SdfEditorWindow::on_remove_volumetric_clicked() {
  QListWidgetItem *item = volumetrics_list_->currentItem();
  if (!item) {
    return;
  }
  int volumetric_index = item->data(Qt::UserRole).toInt();
  if (volumetric_index >= 0 &&
      volumetric_index < static_cast<int>(scene_.volumetrics.size())) {
    scene_.volumetrics.erase(scene_.volumetrics.begin() + volumetric_index);
  }
  refresh_volumetrics_list();
  refresh_material_list();
  sync_viewport_scene();
}

void SdfEditorWindow::on_pick_volumetric_colour_clicked() {
  ScopedRenderPause pause(viewport_);
  QColor picked = QColorDialog::getColor(volumetric_colour_, this,
                                        "Select Colour",
                                        QColorDialog::ShowAlphaChannel);
  if (picked.isValid()) {
    volumetric_colour_ = picked;
    volumetric_colour_button_->setStyleSheet(
        QString("background-color: %1;").arg(volumetric_colour_.name()));
    on_volumetric_field_changed(); // apply immediately if selected
  }
}

void SdfEditorWindow::on_pick_volumetric_texture_clicked() {
  ScopedRenderPause pause(viewport_);
  QString path = QFileDialog::getOpenFileName(
      this, "Select Texture Image", QString(),
      "Images (*.png *.jpg *.jpeg *.bmp *.tga)");
  if (path.isEmpty()) {
    return;
  }

  QImage image(path);
  if (image.isNull()) {
    QMessageBox::warning(this, "Texture Load Failed",
                         "Could not read image: " + path);
    return;
  }

  QDir().mkpath("assets/textures");
  std::string base = sanitize_texture_name(
      QFileInfo(path).completeBaseName().toStdString());
  std::string dest = "assets/textures/" + base + ".png";
  if (!image.save(QString::fromStdString(dest), "PNG")) {
    QMessageBox::warning(this, "Texture Copy Failed",
                         "Could not write " + QString::fromStdString(dest));
    return;
  }

  volumetric_texture_name_ = base;
  volumetric_texture_label_->setText(QString::fromStdString(volumetric_texture_name_));
  on_volumetric_field_changed();
}

void SdfEditorWindow::on_clear_volumetric_texture_clicked() {
  if (volumetric_texture_name_.empty()) {
    return;
  }
  volumetric_texture_name_.clear();
  volumetric_texture_label_->setText("(none)");
  on_volumetric_field_changed();
}

void SdfEditorWindow::on_volumetrics_list_selection_changed() {
  QListWidgetItem *item = volumetrics_list_->currentItem();
  int volumetric_index = item ? item->data(Qt::UserRole).toInt() : -1;
  if (volumetric_index >= 0) {
    populate_volumetric_fields_from_selection(volumetric_index);
  }
}

void SdfEditorWindow::populate_volumetric_fields_from_selection(int volumetric_index) {
  if (volumetric_index < 0 ||
      volumetric_index >= static_cast<int>(scene_.volumetrics.size())) {
    return;
  }
  const SdfVolumetricDef &volumetric = scene_.volumetrics[volumetric_index];
  PrimitiveTypeSpec spec = type_spec_for(volumetric.type);

  populating_volumetric_fields_ = true;

  volumetric_type_list_->setCurrentRow(static_cast<int>(volumetric.type));

  volumetric_pos_x_->setValue(volumetric.position.x);
  volumetric_pos_y_->setValue(volumetric.position.y);
  volumetric_pos_z_->setValue(volumetric.position.z);

  glm::vec3 rotation_degrees = glm::degrees(volumetric.rotation);
  volumetric_rot_x_->setValue(rotation_degrees.x);
  volumetric_rot_y_->setValue(rotation_degrees.y);
  volumetric_rot_z_->setValue(rotation_degrees.z);

  f32 raw_params[4] = {volumetric.params.x, volumetric.params.y,
                       volumetric.params.z, volumetric.extra_param};
  for (size_t i = 0; i < spec.param_labels.size() && i < 4; ++i) {
    volumetric_param_spin_[i]->setValue(raw_params[i]);
  }

  populate_volumetric_fields_from_material(
      sdf_scene_resolve_material(scene_, volumetric));
  volumetric_density_spin_->setValue(volumetric.density);

  populating_volumetric_fields_ = false;

  update_volumetric_field_enablement();
}

void SdfEditorWindow::apply_fields_to_volumetric(int volumetric_index) {
  SdfVolumetricDef &volumetric = scene_.volumetrics[volumetric_index];
  PrimitiveTypeSpec spec = type_spec_for(volumetric.type);

  if (spec.has_position) {
    volumetric.position =
        glm::vec3(static_cast<f32>(volumetric_pos_x_->value()),
                 static_cast<f32>(volumetric_pos_y_->value()),
                 static_cast<f32>(volumetric_pos_z_->value()));
  }
  if (spec.has_rotation) {
    volumetric.rotation =
        glm::radians(glm::vec3(static_cast<f32>(volumetric_rot_x_->value()),
                              static_cast<f32>(volumetric_rot_y_->value()),
                              static_cast<f32>(volumetric_rot_z_->value())));
  }

  f32 raw_params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (size_t i = 0; i < spec.param_labels.size() && i < 4; ++i) {
    raw_params[i] = static_cast<f32>(volumetric_param_spin_[i]->value());
  }
  volumetric.params = glm::vec3(raw_params[0], raw_params[1], raw_params[2]);
  volumetric.extra_param = raw_params[3];
  volumetric.density = static_cast<f32>(volumetric_density_spin_->value());

  volumetric.material_id =
      ensure_volumetric_material_binding(volumetric.material_id);
  volumetric.material_name.clear();
}

void SdfEditorWindow::on_volumetric_field_changed() {
  if (populating_volumetric_fields_ || !volumetrics_list_) {
    return;
  }
  QListWidgetItem *item = volumetrics_list_->currentItem();
  if (!item) {
    return; // nothing selected -- fields are just staging values for Add
  }
  int volumetric_index = item->data(Qt::UserRole).toInt();
  if (volumetric_index < 0 ||
      volumetric_index >= static_cast<int>(scene_.volumetrics.size())) {
    return;
  }
  apply_fields_to_volumetric(volumetric_index);
  request_viewport_resync();
}

void SdfEditorWindow::refresh_volumetrics_list() {
  volumetrics_list_->clear();
  for (int i = 0; i < static_cast<int>(scene_.volumetrics.size()); ++i) {
    const SdfVolumetricDef &volumetric = scene_.volumetrics[i];
    QString text = QString("%1 '%2'")
                       .arg(primitive_type_label(volumetric.type))
                       .arg(QString::fromStdString(volumetric.name));
    auto *item = new QListWidgetItem(text, volumetrics_list_);
    item->setData(Qt::UserRole, i);
  }
}
