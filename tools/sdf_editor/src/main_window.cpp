#include "main_window.h"
#include "contents_tree_widget.h"
#include "scene_viewport.h"
#include <renderer/renderer_frontend.h>
#include <sdf_authoring.h>

#include <QButtonGroup>
#include <QCheckBox>
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
#include <QTabWidget>
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

// contents_tree_'s per-item data roles (see
// SdfEditorWindow::refresh_contents_list()) -- every item (layer or
// primitive) carries kLayerIndexRole; primitive items additionally carry
// kPrimitiveIndexRole (its position within that layer's primitives[]) and
// kPrimitiveNameRole (its stable SdfPrimitiveDef::name, the only thing that
// survives a drag-and-drop reparent -- see
// SdfEditorWindow::sync_layers_from_tree()).
constexpr int kLayerIndexRole = Qt::UserRole;
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

// A material's colour/texture can't be recovered from its material_name
// alone (it's an opaque deterministic hash-ish string, see ensure_material()
// below) -- populate_fields_from_selection() reads the .kmt file itself
// back to recover what colour_/texture_name_ should show for an existing
// selection. Deliberately minimal (unlike MaterialSystem::acquire()'s
// parser, engine-side): this tool only ever reads back files it wrote
// itself via ensure_material(), which are always exactly "key=value" lines
// with no surrounding whitespace.
struct ParsedMaterial {
  QColor colour = Qt::white;
  std::string texture_name;
  std::string bump_map_name; // engine-side Material::bump_map_name -- empty
                            // means no bump map (see Material::bump_texture)
  double texture_scale = 0.6; // engine-side Material::texture_scale default
  // engine-side Material::texture_offset default (world units); rotation
  // kept in RADIANS here too, matching the .kmt file directly -- callers
  // convert to degrees for the UI at the point they call setValue(), the
  // same way a primitive's own rotation already does.
  glm::vec3 texture_offset{0.0f};
  double texture_rotation = 0.0;
  QColor emissive_colour = Qt::white;
  double emissive_intensity = 0.0; // engine-side Material::emissive_intensity
                                  // "off" default
  bool pixelation_exempt = false; // engine-side Material::pixelation_exempt default
};

ParsedMaterial parse_material_file(const std::string &material_name) {
  ParsedMaterial result;
  std::ifstream file("assets/materials/" + material_name + ".kmt");
  std::string line;
  while (std::getline(file, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    if (key == "diffuse_map_name") {
      result.texture_name = value;
    } else if (key == "bump_map_name") {
      result.bump_map_name = value;
    } else if (key == "diffuse_colour") {
      std::istringstream iss(value);
      float r, g, b, a;
      if (iss >> r >> g >> b >> a) {
        result.colour = QColor::fromRgbF(r, g, b, a);
      }
    } else if (key == "texture_scale") {
      std::istringstream iss(value);
      double scale = 0.0;
      if (iss >> scale && scale > 0.0) {
        result.texture_scale = scale;
      }
    } else if (key == "texture_offset") {
      std::istringstream iss(value);
      glm::vec3 offset(0.0f);
      if (iss >> offset.x >> offset.y >> offset.z) {
        result.texture_offset = offset;
      }
    } else if (key == "texture_rotation") {
      std::istringstream iss(value);
      double rotation = 0.0;
      if (iss >> rotation) {
        result.texture_rotation = rotation;
      }
    } else if (key == "emissive_colour" || key == "emissive_color") {
      std::istringstream iss(value);
      float r, g, b;
      if (iss >> r >> g >> b) {
        result.emissive_colour = QColor::fromRgbF(r, g, b);
      }
    } else if (key == "emissive_intensity") {
      std::istringstream iss(value);
      double intensity = 0.0;
      if (iss >> intensity && intensity >= 0.0) {
        result.emissive_intensity = intensity;
      }
    } else if (key == "pixelation_exempt") {
      result.pixelation_exempt = (value == "true" || value == "1");
    }
  }
  return result;
}

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
};

PrimitiveTypeSpec type_spec_for(SdfPrimitiveType type) {
  switch (type) {
  case SdfPrimitiveType::Sphere:
    return {true, true, {"Radius"}};
  case SdfPrimitiveType::Box:
    return {true, true, {"Half-Extent X", "Half-Extent Y", "Half-Extent Z"}};
  case SdfPrimitiveType::Plane:
    return {false, false, {"Height"}};
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
    return {true, true, {"Height"}};
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
  gizmo_mode_row->addStretch(/*stretch=*/1);
  middle_panel->addLayout(gizmo_mode_row);
  middle_panel->addWidget(viewport_container, /*stretch=*/1);
  root_layout->addLayout(middle_panel, /*stretch=*/3);

  // Right panel: how to join it in, its transform, its colour, and the
  // running scene contents.
  auto *right_panel = new QVBoxLayout();

  auto *primitives_tab = new QWidget();
  auto *primitives_layout = new QVBoxLayout(primitives_tab);

  QFormLayout *form = add_collapsible_section(primitives_layout, "Layer");

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

  form = add_collapsible_section(primitives_layout, "Transform");

  pos_x_ = new QDoubleSpinBox();
  pos_y_ = new QDoubleSpinBox();
  pos_z_ = new QDoubleSpinBox();
  for (QDoubleSpinBox *spin : {pos_x_, pos_y_, pos_z_}) {
    spin->setRange(-100.0, 100.0);
    spin->setSingleStep(0.1);
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
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
           &SdfEditorWindow::on_live_edit_changed);
  }
  auto *rot_row = new QHBoxLayout();
  rot_row->addWidget(rot_x_);
  rot_row->addWidget(rot_y_);
  rot_row->addWidget(rot_z_);
  form->addRow("Rotation (x, y, z):", rot_row);

  form = add_collapsible_section(primitives_layout, "Repetition", /*expanded=*/false);

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

  form = add_collapsible_section(primitives_layout, "Deformation", /*expanded=*/false);

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
      "Radians of rotation per world-unit of local X, around local Z -- "
      "bends the shape along X, applied after Twist. 0 = no bend.");
  connect(bend_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
         &SdfEditorWindow::on_live_edit_changed);
  form->addRow("Bend:", bend_spin_);

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

  form = add_collapsible_section(primitives_layout, "Shape Parameters");

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

  form = add_collapsible_section(primitives_layout, "Material && Texture");

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

  form = add_collapsible_section(primitives_layout, "Emissive && Rendering", /*expanded=*/false);

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

  auto *layer_clipboard_row = new QHBoxLayout();
  copy_layer_button_ = new QPushButton("Copy Layer");
  copy_layer_button_->setToolTip(
      "Copies the selected row's layer (every primitive in it, deep-copied) "
      "onto an in-memory clipboard -- select any primitive within a layer, "
      "or the layer row itself.");
  connect(copy_layer_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_copy_layer_clicked);
  layer_clipboard_row->addWidget(copy_layer_button_);
  paste_layer_button_ = new QPushButton("Paste Layer");
  paste_layer_button_->setEnabled(false); // enabled once something's been copied
  paste_layer_button_->setToolTip(
      "Appends a fresh copy of the last-copied layer -- every primitive "
      "gets a newly generated unique name, so pasting repeatedly never "
      "collides with the original or an earlier paste.");
  connect(paste_layer_button_, &QPushButton::clicked, this,
         &SdfEditorWindow::on_paste_layer_clicked);
  layer_clipboard_row->addWidget(paste_layer_button_);
  primitives_layout->addLayout(layer_clipboard_row);

  primitives_layout->addWidget(new QLabel("Scene Contents"));
  contents_tree_ = new ContentsTreeWidget();
  contents_tree_->setHeaderHidden(true);
  connect(contents_tree_, &QTreeWidget::itemSelectionChanged, this,
         &SdfEditorWindow::on_contents_tree_selection_changed);
  connect(contents_tree_, &ContentsTreeWidget::primitives_reparented, this,
         &SdfEditorWindow::on_primitives_reparented);
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

  auto *tabs = new QTabWidget();
  tabs->addTab(primitives_tab, "Primitives");
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

  repeat_cell_x_->setEnabled(cell_relevant);
  repeat_cell_y_->setEnabled(cell_relevant && !is_rectangular); // Rectangular locks Y
  repeat_cell_z_->setEnabled(cell_relevant);
  repeat_count_x_->setEnabled(count_relevant); // also n for Rotational
  repeat_count_y_->setEnabled(count_relevant && !is_rotational && !is_rectangular);
  repeat_count_z_->setEnabled(count_relevant && !is_rotational);
}

std::string SdfEditorWindow::ensure_material() const {
  // Scale folded into the name as centi-units (0.6 -> "ts060") -- it has
  // to participate in the deterministic name for the same reason the
  // colour does: GeometrySystem/MaterialSystem cache materials by name, so
  // two primitives differing *only* in texture scale would otherwise
  // collide on one cached entry and silently share whichever scale was
  // written first.
  int scale_centi =
      static_cast<int>(std::lround(texture_scale_spin_->value() * 100.0));
  double emissive_intensity = emissive_intensity_spin_->value();
  // Same reasoning as scale_centi above, but only appended when actually
  // emissive -- keeps every pre-existing (non-emissive) material's name
  // unchanged, so this feature can't retroactively fragment materials
  // authored before it existed.
  char emissive_suffix[48] = "";
  if (emissive_intensity > 0.0) {
    int intensity_centi = static_cast<int>(std::lround(emissive_intensity * 100.0));
    std::snprintf(emissive_suffix, sizeof(emissive_suffix), "_em%04d%02x%02x%02x",
                 intensity_centi, emissive_colour_.red(),
                 emissive_colour_.green(), emissive_colour_.blue());
  }
  bool pixelation_exempt = pixelation_exempt_check_->isChecked();
  const char *pixelation_suffix = pixelation_exempt ? "_px" : "";

  // Same reasoning as emissive_suffix above -- only appended when actually
  // non-default, so a primitive that never touches offset/rotation keeps
  // exactly the material name it always would have.
  glm::vec3 offset(texture_offset_x_->value(), texture_offset_y_->value(),
                   texture_offset_z_->value());
  double rotation_degrees = texture_rotation_spin_->value();
  char tex_transform_suffix[64] = "";
  if (offset != glm::vec3(0.0f) || rotation_degrees != 0.0) {
    std::snprintf(tex_transform_suffix, sizeof(tex_transform_suffix),
                 "_to%+04d%+04d%+04d_tr%+05d",
                 static_cast<int>(std::lround(offset.x * 100.0)),
                 static_cast<int>(std::lround(offset.y * 100.0)),
                 static_cast<int>(std::lround(offset.z * 100.0)),
                 static_cast<int>(std::lround(rotation_degrees * 100.0)));
  }

  // Same reasoning as tex_transform_suffix above -- only appended when a
  // bump map is actually set, so a primitive that never touches it keeps
  // exactly the material name it always would have.
  char bump_suffix[160] = "";
  if (!bump_map_name_.empty()) {
    std::snprintf(bump_suffix, sizeof(bump_suffix), "_bump_%s",
                 bump_map_name_.c_str());
  }

  char name_buf[384];
  if (texture_name_.empty()) {
    std::snprintf(name_buf, sizeof(name_buf),
                 "qt_colour_%02x%02x%02x%02x_ts%03d%s%s%s%s", colour_.red(),
                 colour_.green(), colour_.blue(), colour_.alpha(),
                 scale_centi, emissive_suffix, pixelation_suffix,
                 tex_transform_suffix, bump_suffix);
  } else {
    std::snprintf(name_buf, sizeof(name_buf),
                 "qt_colour_%02x%02x%02x%02x_ts%03d%s%s%s%s_%s", colour_.red(),
                 colour_.green(), colour_.blue(), colour_.alpha(),
                 scale_centi, emissive_suffix, pixelation_suffix,
                 tex_transform_suffix, bump_suffix, texture_name_.c_str());
  }
  std::string name = name_buf;

  // Deterministic from the colour's RGBA, texture scale/offset/rotation,
  // emissive colour/intensity, pixelation-exempt flag, texture name, and
  // bump map name, so picking the same combination again later just reuses
  // this file instead of accumulating duplicates.
  std::ofstream file("assets/materials/" + name + ".kmt");
  if (file.is_open()) {
    file << "#material file\n\n";
    file << "version=0.1\n";
    file << "name=" << name << "\n";
    file << "diffuse_colour=" << colour_.redF() << " " << colour_.greenF()
        << " " << colour_.blueF() << " " << colour_.alphaF() << "\n";
    file << "texture_scale=" << texture_scale_spin_->value() << "\n";
    if (offset != glm::vec3(0.0f)) {
      file << "texture_offset=" << offset.x << " " << offset.y << " "
          << offset.z << "\n";
    }
    if (rotation_degrees != 0.0) {
      file << "texture_rotation=" << glm::radians(rotation_degrees) << "\n";
    }
    if (emissive_intensity > 0.0) {
      file << "emissive_colour=" << emissive_colour_.redF() << " "
          << emissive_colour_.greenF() << " " << emissive_colour_.blueF()
          << "\n";
      file << "emissive_intensity=" << emissive_intensity << "\n";
    }
    // Otherwise no emissive_intensity line -- MaterialSystem's default (0)
    // means "not emissive", same convention every other optional field
    // here uses.
    if (pixelation_exempt) {
      file << "pixelation_exempt=true\n";
    }
    if (!texture_name_.empty()) {
      file << "diffuse_map_name=" << texture_name_ << "\n";
    }
    // Otherwise no diffuse_map_name -- MaterialSystem falls back to the
    // default (checkerboard) texture, tinted by diffuse_colour above, same
    // convention assets/materials/default_text_material.kmt already uses.
    if (!bump_map_name_.empty()) {
      file << "bump_map_name=" << bump_map_name_ << "\n";
    }
    // Otherwise no bump_map_name -- MaterialSystem falls back to
    // TextureSystem::flat_texture(), i.e. no bump mapping at all.
  }
  return name;
}

void SdfEditorWindow::on_add_clicked() {
  int row = type_list_->currentRow();
  if (row < 0) {
    return;
  }
  SdfPrimitiveType type = static_cast<SdfPrimitiveType>(row);
  PrimitiveTypeSpec spec = type_spec_for(type);

  std::string material_name = ensure_material();

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
                       material_name);
  } else if (type == SdfPrimitiveType::Box) {
    added = &add_box(layer, primitive_name, position, rotation, params,
                    material_name);
  } else if (type == SdfPrimitiveType::Plane) {
    added = &add_plane(layer, primitive_name, params.x,
                      material_name); // params.x = height
  } else {
    added = &add_primitive(layer, primitive_name, type, position, rotation,
                          params, extra_param, material_name);
  }

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

void SdfEditorWindow::on_copy_layer_clicked() {
  QList<QTreeWidgetItem *> selected = contents_tree_->selectedItems();
  if (selected.isEmpty()) {
    return;
  }

  // Same "touched layers" rule on_remove_clicked() uses: a selected layer
  // row contributes itself, a selected primitive row contributes its parent
  // layer -- a std::set both dedupes (several primitives in one layer still
  // copy it once) and gives ascending scene_.layers index order for free.
  std::set<int> touched_layers;
  for (QTreeWidgetItem *item : selected) {
    touched_layers.insert(item->parent()
                               ? item->parent()->data(0, kLayerIndexRole).toInt()
                               : item->data(0, kLayerIndexRole).toInt());
  }

  layer_clipboard_.clear();
  layer_clipboard_.reserve(touched_layers.size());
  for (int index : touched_layers) {
    layer_clipboard_.push_back(scene_.layers[index]); // deep copy --
                                                       // SdfLayerDef owns
                                                       // its primitives[]
                                                       // outright
  }
  paste_layer_button_->setEnabled(true);
}

void SdfEditorWindow::on_paste_layer_clicked() {
  if (layer_clipboard_.empty()) {
    return;
  }
  // Deep copy, then rename the layer AND every primitive inside it to a
  // fresh, globally unique name -- GeometrySystem::acquire() (engine-side)
  // keys purely off name, so pasting the clipboard's names verbatim would
  // silently bump the *original* layer/primitives' reference counts instead
  // of registering new geometry, discarding whichever position/params the
  // pasted copy was actually given (see on_add_clicked()'s own comment on
  // next_primitive_id_ for the exact same hazard). Reusing next_layer_id_/
  // next_primitive_id_ -- the same monotonic counters on_new_layer_clicked()/
  // on_add_clicked() already draw from -- keeps every name in the scene
  // unique regardless of whether it came from Add, New Layer, or Paste.
  int first_pasted_index = static_cast<int>(scene_.layers.size());
  for (const SdfLayerDef &copied : layer_clipboard_) {
    SdfLayerDef pasted = copied;
    pasted.name = "layer" + std::to_string(next_layer_id_++);
    for (SdfPrimitiveDef &primitive : pasted.primitives) {
      primitive.name = "primitive" + std::to_string(next_primitive_id_++);
    }
    scene_.layers.push_back(std::move(pasted));
  }

  refresh_contents_list();
  sync_viewport_scene();

  // Select every newly pasted layer row -- mirrors on_new_layer_clicked()'s
  // own selection of a freshly added layer, extended to the whole batch.
  contents_tree_->clearSelection();
  QTreeWidgetItem *first_pasted_item = nullptr;
  for (int i = 0; i < contents_tree_->topLevelItemCount(); ++i) {
    QTreeWidgetItem *layer_item = contents_tree_->topLevelItem(i);
    if (layer_item->data(0, kLayerIndexRole).toInt() >= first_pasted_index) {
      layer_item->setSelected(true);
      if (!first_pasted_item) {
        first_pasted_item = layer_item;
      }
    }
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
  {
    const QSignalBlocker blocker(ambient_spin_);
    ambient_spin_->setValue(scene_.ambient);
  }
  sync_viewport_scene();
  active_layer_index_ = -1;
  viewport_->set_selection({}); // previous selection is from a different
                                // scene entirely now
}

void SdfEditorWindow::sync_viewport_scene() {
  viewport_->set_scene(scene_); // keeps click-picking in sync too

  if (!save_scene(kLivePreviewPath, scene_)) {
    return; // save_scene() already logged why.
  }
  // The editor's entire authored world *is* scene_, so clearing every
  // loaded scene and reloading it whole is correct, not just convenient --
  // there's nothing else this tool ever loads alongside it.
  renderer_clear_scenes();
  renderer_load_scene(kLivePreviewPath);
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
  // Persists + rebakes, and re-pushes scene_ into viewport_ -- resolving
  // the temporary divergence between SdfEditorWindow's and SceneViewport's
  // copies that existed only during the drag itself.
  sync_viewport_scene();
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
    populate_fields_from_selection(selection.front().layer_index,
                                   selection.front().primitive_index);
    return;
  }
  if (!selection.empty()) {
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
  } else {
    active_layer_index_ = -1;
  }
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
    // Layer identity (name/operation/smoothness) is untouched by a
    // primitive drag -- only its primitives[] is being rebuilt here.
    SdfLayerDef new_layer;
    new_layer.name = scene_.layers[old_layer_index].name;
    new_layer.operation = scene_.layers[old_layer_index].operation;
    new_layer.smoothness = scene_.layers[old_layer_index].smoothness;

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
  displace_amplitude_spin_->setValue(primitive.displace_amplitude);
  displace_frequency_spin_->setValue(primitive.displace_frequency);

  ParsedMaterial material = parse_material_file(primitive.material_name);
  colour_ = material.colour;
  texture_name_ = material.texture_name;
  colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(colour_.name()));
  texture_label_->setText(texture_name_.empty()
                              ? QStringLiteral("(none)")
                              : QString::fromStdString(texture_name_));
  bump_map_name_ = material.bump_map_name;
  bump_map_label_->setText(bump_map_name_.empty()
                               ? QStringLiteral("(none)")
                               : QString::fromStdString(bump_map_name_));
  texture_scale_spin_->setValue(material.texture_scale);
  texture_offset_x_->setValue(material.texture_offset.x);
  texture_offset_y_->setValue(material.texture_offset.y);
  texture_offset_z_->setValue(material.texture_offset.z);
  texture_rotation_spin_->setValue(glm::degrees(material.texture_rotation));
  emissive_colour_ = material.emissive_colour;
  emissive_colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(emissive_colour_.name()));
  emissive_intensity_spin_->setValue(material.emissive_intensity);
  pixelation_exempt_check_->setChecked(material.pixelation_exempt);

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
  primitive.displace_amplitude = static_cast<f32>(displace_amplitude_spin_->value());
  primitive.displace_frequency = static_cast<f32>(displace_frequency_spin_->value());

  primitive.material_name = ensure_material();
}

void SdfEditorWindow::on_live_edit_changed() {
  if (populating_fields_ || !contents_tree_) {
    // !contents_tree_: a field's valueChanged can fire from the
    // constructor itself (setting an initial default after the signal is
    // already connected), before contents_tree_ exists yet.
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
  sync_viewport_scene();
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
  sync_viewport_scene();
  // Keeps the gizmo in sync with a Type toggle -- e.g. switching from Point
  // to Directional should hide it (a directional light has no position for
  // the gizmo to show), and the reverse should show it again. A no-op
  // (recomputes the same selection) for any other field's change.
  update_viewport_light_selection(light_index);
}

void SdfEditorWindow::on_ambient_changed() {
  scene_.ambient = static_cast<f32>(ambient_spin_->value());
  sync_viewport_scene();
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

void SdfEditorWindow::refresh_contents_list() {
  contents_tree_->clear();
  for (int i = 0; i < static_cast<int>(scene_.layers.size()); ++i) {
    const SdfLayerDef &layer = scene_.layers[i];
    const char *op_label =
        layer.operation == SdfLayerOperation::Subtraction ? "Subtract" : "Union";
    QString layer_text = QString("%1 [%2, smoothness %3]")
                             .arg(QString::fromStdString(layer.name))
                             .arg(op_label)
                             .arg(layer.smoothness, 0, 'f', 2);
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

std::string SdfEditorWindow::ensure_volumetric_material() const {
  // Mirrors ensure_material()'s deterministic-name idiom, but simpler: a
  // volumetric material never has emissive/pixelation-exempt settings, so
  // there's no equivalent suffix to fold in here.
  int scale_centi = static_cast<int>(
      std::lround(volumetric_texture_scale_spin_->value() * 100.0));

  glm::vec3 offset(volumetric_texture_offset_x_->value(),
                   volumetric_texture_offset_y_->value(),
                   volumetric_texture_offset_z_->value());
  double rotation_degrees = volumetric_texture_rotation_spin_->value();
  char tex_transform_suffix[64] = "";
  if (offset != glm::vec3(0.0f) || rotation_degrees != 0.0) {
    std::snprintf(tex_transform_suffix, sizeof(tex_transform_suffix),
                 "_to%+04d%+04d%+04d_tr%+05d",
                 static_cast<int>(std::lround(offset.x * 100.0)),
                 static_cast<int>(std::lround(offset.y * 100.0)),
                 static_cast<int>(std::lround(offset.z * 100.0)),
                 static_cast<int>(std::lround(rotation_degrees * 100.0)));
  }

  char name_buf[224];
  if (volumetric_texture_name_.empty()) {
    std::snprintf(name_buf, sizeof(name_buf), "qt_vol_colour_%02x%02x%02x%02x_ts%03d%s",
                 volumetric_colour_.red(), volumetric_colour_.green(),
                 volumetric_colour_.blue(), volumetric_colour_.alpha(), scale_centi,
                 tex_transform_suffix);
  } else {
    std::snprintf(name_buf, sizeof(name_buf),
                 "qt_vol_colour_%02x%02x%02x%02x_ts%03d%s_%s",
                 volumetric_colour_.red(), volumetric_colour_.green(),
                 volumetric_colour_.blue(), volumetric_colour_.alpha(), scale_centi,
                 tex_transform_suffix, volumetric_texture_name_.c_str());
  }
  std::string name = name_buf;

  std::ofstream file("assets/materials/" + name + ".kmt");
  if (file.is_open()) {
    file << "#material file\n\n";
    file << "version=0.1\n";
    file << "name=" << name << "\n";
    file << "diffuse_colour=" << volumetric_colour_.redF() << " "
        << volumetric_colour_.greenF() << " " << volumetric_colour_.blueF()
        << " " << volumetric_colour_.alphaF() << "\n";
    file << "texture_scale=" << volumetric_texture_scale_spin_->value() << "\n";
    if (offset != glm::vec3(0.0f)) {
      file << "texture_offset=" << offset.x << " " << offset.y << " "
          << offset.z << "\n";
    }
    if (rotation_degrees != 0.0) {
      file << "texture_rotation=" << glm::radians(rotation_degrees) << "\n";
    }
    if (!volumetric_texture_name_.empty()) {
      file << "diffuse_map_name=" << volumetric_texture_name_ << "\n";
    }
  }
  return name;
}

void SdfEditorWindow::on_add_volumetric_clicked() {
  int row = volumetric_type_list_->currentRow();
  if (row < 0) {
    return;
  }
  SdfPrimitiveType type = static_cast<SdfPrimitiveType>(row);
  PrimitiveTypeSpec spec = type_spec_for(type);

  std::string material_name = ensure_volumetric_material();
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
                 density, material_name);

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

  ParsedMaterial material = parse_material_file(volumetric.material_name);
  volumetric_colour_ = material.colour;
  volumetric_texture_name_ = material.texture_name;
  volumetric_colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(volumetric_colour_.name()));
  volumetric_texture_label_->setText(
      volumetric_texture_name_.empty()
          ? QStringLiteral("(none)")
          : QString::fromStdString(volumetric_texture_name_));
  volumetric_texture_scale_spin_->setValue(material.texture_scale);
  volumetric_texture_offset_x_->setValue(material.texture_offset.x);
  volumetric_texture_offset_y_->setValue(material.texture_offset.y);
  volumetric_texture_offset_z_->setValue(material.texture_offset.z);
  volumetric_texture_rotation_spin_->setValue(glm::degrees(material.texture_rotation));
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

  volumetric.material_name = ensure_volumetric_material();
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
  sync_viewport_scene();
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
