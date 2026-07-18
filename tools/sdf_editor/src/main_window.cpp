#include "main_window.h"
#include "scene_viewport.h"
#include <renderer/renderer_frontend.h>
#include <sdf_authoring.h>

#include <QButtonGroup>
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
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace {
// Fixed path renderer_load_scene() re-reads every time scene_ changes --
// see sync_viewport_scene(). Not the same as the user-chosen Save Scene...
// path; this one is purely an implementation detail of keeping the
// viewport live.
constexpr std::string_view kLivePreviewPath = "assets/scenes/.sdf_editor_live.sdf";

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
  double texture_scale = 0.6; // engine-side Material::texture_scale default
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
  connect(viewport_, &SceneViewport::primitive_picked, this,
         &SdfEditorWindow::on_viewport_primitive_picked);
  connect(viewport_, &SceneViewport::primitive_transformed, this,
         &SdfEditorWindow::on_viewport_primitive_transformed);
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

  auto *form_group = new QGroupBox("New Primitive");
  auto *form = new QFormLayout(form_group);

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

  auto *primitives_tab = new QWidget();
  auto *primitives_layout = new QVBoxLayout(primitives_tab);
  primitives_layout->addWidget(form_group);

  auto *add_button = new QPushButton("Add Primitive");
  connect(add_button, &QPushButton::clicked, this,
         &SdfEditorWindow::on_add_clicked);
  primitives_layout->addWidget(add_button);

  primitives_layout->addWidget(new QLabel("Scene Contents"));
  contents_list_ = new QListWidget();
  contents_list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  connect(contents_list_, &QListWidget::currentItemChanged, this,
         &SdfEditorWindow::on_contents_list_selection_changed);
  primitives_layout->addWidget(contents_list_, /*stretch=*/1);

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

  auto *tabs = new QTabWidget();
  tabs->addTab(primitives_tab, "Primitives");
  tabs->addTab(lights_tab, "Lights");
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
  char name_buf[96];
  if (texture_name_.empty()) {
    std::snprintf(name_buf, sizeof(name_buf),
                 "qt_colour_%02x%02x%02x%02x_ts%03d", colour_.red(),
                 colour_.green(), colour_.blue(), colour_.alpha(),
                 scale_centi);
  } else {
    std::snprintf(name_buf, sizeof(name_buf),
                 "qt_colour_%02x%02x%02x%02x_ts%03d_%s", colour_.red(),
                 colour_.green(), colour_.blue(), colour_.alpha(),
                 scale_centi, texture_name_.c_str());
  }
  std::string name = name_buf;

  // Deterministic from the colour's RGBA, texture scale and texture name,
  // so picking the same combination again later just reuses this file
  // instead of accumulating duplicates.
  std::ofstream file("assets/materials/" + name + ".kmt");
  if (file.is_open()) {
    file << "#material file\n\n";
    file << "version=0.1\n";
    file << "name=" << name << "\n";
    file << "diffuse_colour=" << colour_.redF() << " " << colour_.greenF()
        << " " << colour_.blueF() << " " << colour_.alphaF() << "\n";
    file << "texture_scale=" << texture_scale_spin_->value() << "\n";
    if (!texture_name_.empty()) {
      file << "diffuse_map_name=" << texture_name_ << "\n";
    }
    // Otherwise no diffuse_map_name -- MaterialSystem falls back to the
    // default (checkerboard) texture, tinted by diffuse_colour above, same
    // convention assets/materials/default_text_material.kmt already uses.
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

  SdfLayerOperation operation = operation_combo_->currentIndex() == 1
                                    ? SdfLayerOperation::Subtraction
                                    : SdfLayerOperation::Union;
  f32 smoothness = static_cast<f32>(smoothness_spin_->value());

  std::string layer_name = "layer" + std::to_string(next_layer_id_++);
  SdfLayerDef &layer = add_layer(scene_, layer_name, operation, smoothness);
  std::string primitive_name = layer_name + "_primitive";

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

  refresh_contents_list();
  sync_viewport_scene();
}

void SdfEditorWindow::on_remove_clicked() {
  QListWidgetItem *item = contents_list_->currentItem();
  if (!item) {
    return;
  }
  int layer_index = item->data(Qt::UserRole).toInt();
  if (layer_index >= 0 &&
      layer_index < static_cast<int>(scene_.layers.size())) {
    scene_.layers.erase(scene_.layers.begin() + layer_index);
  }
  refresh_contents_list();
  sync_viewport_scene();
  // Layer indices may have shifted (or the removed one no longer exists at
  // all) -- any previous selection is potentially stale/wrong now.
  viewport_->set_selected_layer(-1);
}

void SdfEditorWindow::on_pick_colour_clicked() {
  QColor picked = QColorDialog::getColor(colour_, this, "Select Colour",
                                         QColorDialog::ShowAlphaChannel);
  if (picked.isValid()) {
    colour_ = picked;
    colour_button_->setStyleSheet(
        QString("background-color: %1;").arg(colour_.name()));
    on_live_edit_changed(); // apply immediately if a primitive is selected
  }
}

void SdfEditorWindow::on_pick_texture_clicked() {
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

  refresh_contents_list();
  refresh_lights_list();
  {
    const QSignalBlocker blocker(ambient_spin_);
    ambient_spin_->setValue(scene_.ambient);
  }
  sync_viewport_scene();
  viewport_->set_selected_layer(-1); // previous selection is from a
                                     // different scene entirely now
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

void SdfEditorWindow::on_viewport_primitive_picked(int layer_index) {
  if (layer_index < 0) {
    contents_list_->clearSelection();
    contents_list_->setCurrentItem(nullptr);
    return;
  }
  for (int i = 0; i < contents_list_->count(); ++i) {
    QListWidgetItem *item = contents_list_->item(i);
    if (item->data(Qt::UserRole).toInt() == layer_index) {
      contents_list_->setCurrentItem(item);
      break;
    }
  }
}

void SdfEditorWindow::on_viewport_primitive_transformed(int layer_index,
                                                        glm::vec3 position,
                                                        glm::vec3 rotation,
                                                        glm::vec3 params) {
  if (layer_index < 0 || layer_index >= static_cast<int>(scene_.layers.size())) {
    return;
  }
  auto &primitives = scene_.layers[layer_index].primitives;
  if (primitives.empty()) {
    return;
  }
  primitives.front().position = position;
  primitives.front().rotation = rotation;
  primitives.front().params = params;
  // Persists + rebakes, and re-pushes scene_ into viewport_ -- resolving
  // the temporary divergence between SdfEditorWindow's and SceneViewport's
  // copies that existed only during the drag itself.
  sync_viewport_scene();
  // The drag just changed position/rotation/size without going through the
  // side panel's fields at all -- refresh them so they don't show stale
  // pre-drag values.
  populate_fields_from_selection(layer_index);
}

void SdfEditorWindow::on_contents_list_selection_changed() {
  QListWidgetItem *item = contents_list_->currentItem();
  int layer_index = item ? item->data(Qt::UserRole).toInt() : -1;
  viewport_->set_selected_layer(layer_index);
  if (layer_index >= 0) {
    populate_fields_from_selection(layer_index);
  }
}

void SdfEditorWindow::populate_fields_from_selection(int layer_index) {
  if (layer_index < 0 || layer_index >= static_cast<int>(scene_.layers.size())) {
    return;
  }
  const SdfLayerDef &layer = scene_.layers[layer_index];
  if (layer.primitives.empty()) {
    return;
  }
  const SdfPrimitiveDef &primitive = layer.primitives.front();
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

  ParsedMaterial material = parse_material_file(primitive.material_name);
  colour_ = material.colour;
  texture_name_ = material.texture_name;
  colour_button_->setStyleSheet(
      QString("background-color: %1;").arg(colour_.name()));
  texture_label_->setText(texture_name_.empty()
                              ? QStringLiteral("(none)")
                              : QString::fromStdString(texture_name_));
  texture_scale_spin_->setValue(material.texture_scale);

  populating_fields_ = false;

  update_field_enablement();
}

void SdfEditorWindow::apply_fields_to_primitive(int layer_index) {
  SdfLayerDef &layer = scene_.layers[layer_index];
  if (layer.primitives.empty()) {
    return;
  }
  SdfPrimitiveDef &primitive = layer.primitives.front();
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

  primitive.material_name = ensure_material();
}

void SdfEditorWindow::on_live_edit_changed() {
  if (populating_fields_ || !contents_list_) {
    // !contents_list_: a field's valueChanged can fire from the
    // constructor itself (setting an initial default after the signal is
    // already connected), before contents_list_ exists yet.
    return;
  }
  QListWidgetItem *item = contents_list_->currentItem();
  if (!item) {
    return; // nothing selected -- fields are just staging values for Add
  }
  int layer_index = item->data(Qt::UserRole).toInt();
  if (layer_index < 0 || layer_index >= static_cast<int>(scene_.layers.size())) {
    return;
  }
  apply_fields_to_primitive(layer_index);
  sync_viewport_scene();
}

void SdfEditorWindow::on_param_expr_changed() {
  update_field_enablement(); // re-grey param_spin_[i] to match which
                            // param_expr_edit_[i] now have text
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
}

void SdfEditorWindow::on_pick_light_colour_clicked() {
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
  contents_list_->clear();
  for (int i = 0; i < static_cast<int>(scene_.layers.size()); ++i) {
    const SdfLayerDef &layer = scene_.layers[i];
    const char *op_label =
        layer.operation == SdfLayerOperation::Subtraction ? "Subtract" : "Union";
    for (const SdfPrimitiveDef &primitive : layer.primitives) {
      QString text = QString("%1 '%2' [%3]")
                         .arg(primitive_type_label(primitive.type))
                         .arg(QString::fromStdString(primitive.name))
                         .arg(op_label);
      auto *item = new QListWidgetItem(text, contents_list_);
      item->setData(Qt::UserRole, i);
    }
  }
}
