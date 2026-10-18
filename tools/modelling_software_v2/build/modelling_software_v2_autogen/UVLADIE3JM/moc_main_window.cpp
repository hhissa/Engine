/****************************************************************************
** Meta object code from reading C++ file 'main_window.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.4.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../src/main_window.h"
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'main_window.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 68
#error "This file was generated using the moc from 6.4.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
namespace {
struct qt_meta_stringdata_ModellingWindow_t {
    uint offsetsAndSizes[100];
    char stringdata0[16];
    char stringdata1[16];
    char stringdata2[1];
    char stringdata3[10];
    char stringdata4[7];
    char stringdata5[17];
    char stringdata6[11];
    char stringdata7[10];
    char stringdata8[7];
    char stringdata9[4];
    char stringdata10[11];
    char stringdata11[14];
    char stringdata12[3];
    char stringdata13[14];
    char stringdata14[10];
    char stringdata15[18];
    char stringdata16[18];
    char stringdata17[18];
    char stringdata18[18];
    char stringdata19[16];
    char stringdata20[12];
    char stringdata21[17];
    char stringdata22[6];
    char stringdata23[14];
    char stringdata24[22];
    char stringdata25[6];
    char stringdata26[16];
    char stringdata27[21];
    char stringdata28[21];
    char stringdata29[21];
    char stringdata30[21];
    char stringdata31[18];
    char stringdata32[32];
    char stringdata33[17];
    char stringdata34[26];
    char stringdata35[22];
    char stringdata36[8];
    char stringdata37[28];
    char stringdata38[24];
    char stringdata39[22];
    char stringdata40[25];
    char stringdata41[21];
    char stringdata42[23];
    char stringdata43[23];
    char stringdata44[21];
    char stringdata45[14];
    char stringdata46[11];
    char stringdata47[16];
    char stringdata48[18];
    char stringdata49[25];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_ModellingWindow_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_ModellingWindow_t qt_meta_stringdata_ModellingWindow = {
    {
        QT_MOC_LITERAL(0, 15),  // "ModellingWindow"
        QT_MOC_LITERAL(16, 15),  // "on_stamp_placed"
        QT_MOC_LITERAL(32, 0),  // ""
        QT_MOC_LITERAL(33, 9),  // "glm::vec3"
        QT_MOC_LITERAL(43, 6),  // "center"
        QT_MOC_LITERAL(50, 16),  // "SculptBrushShape"
        QT_MOC_LITERAL(67, 10),  // "shape_type"
        QT_MOC_LITERAL(78, 9),  // "glm::vec4"
        QT_MOC_LITERAL(88, 6),  // "params"
        QT_MOC_LITERAL(95, 3),  // "f32"
        QT_MOC_LITERAL(99, 10),  // "smoothness"
        QT_MOC_LITERAL(110, 13),  // "SculptBrushOp"
        QT_MOC_LITERAL(124, 2),  // "op"
        QT_MOC_LITERAL(127, 13),  // "material_slot"
        QT_MOC_LITERAL(141, 9),  // "looseness"
        QT_MOC_LITERAL(151, 17),  // "on_save_triggered"
        QT_MOC_LITERAL(169, 17),  // "on_load_triggered"
        QT_MOC_LITERAL(187, 17),  // "on_undo_triggered"
        QT_MOC_LITERAL(205, 17),  // "on_redo_triggered"
        QT_MOC_LITERAL(223, 15),  // "on_stroke_ended"
        QT_MOC_LITERAL(239, 11),  // "stamp_count"
        QT_MOC_LITERAL(251, 16),  // "on_shape_changed"
        QT_MOC_LITERAL(268, 5),  // "index"
        QT_MOC_LITERAL(274, 13),  // "on_op_changed"
        QT_MOC_LITERAL(288, 21),  // "on_smoothness_changed"
        QT_MOC_LITERAL(310, 5),  // "value"
        QT_MOC_LITERAL(316, 15),  // "on_size_changed"
        QT_MOC_LITERAL(332, 20),  // "on_looseness_changed"
        QT_MOC_LITERAL(353, 20),  // "on_stretch_x_changed"
        QT_MOC_LITERAL(374, 20),  // "on_stretch_y_changed"
        QT_MOC_LITERAL(395, 20),  // "on_stretch_z_changed"
        QT_MOC_LITERAL(416, 17),  // "on_finish_changed"
        QT_MOC_LITERAL(434, 31),  // "on_pick_material_colour_clicked"
        QT_MOC_LITERAL(466, 16),  // "on_fleck_changed"
        QT_MOC_LITERAL(483, 25),  // "on_fleck_strength_changed"
        QT_MOC_LITERAL(509, 21),  // "on_clone_mode_toggled"
        QT_MOC_LITERAL(531, 7),  // "checked"
        QT_MOC_LITERAL(539, 27),  // "on_set_clone_source_clicked"
        QT_MOC_LITERAL(567, 23),  // "on_clone_source_missing"
        QT_MOC_LITERAL(591, 21),  // "on_spray_mode_toggled"
        QT_MOC_LITERAL(613, 24),  // "on_spray_opacity_changed"
        QT_MOC_LITERAL(638, 20),  // "on_move_mode_toggled"
        QT_MOC_LITERAL(659, 22),  // "on_move_source_missing"
        QT_MOC_LITERAL(682, 22),  // "on_delete_edit_clicked"
        QT_MOC_LITERAL(705, 20),  // "on_move_edit_clicked"
        QT_MOC_LITERAL(726, 13),  // "on_edit_moved"
        QT_MOC_LITERAL(740, 10),  // "new_center"
        QT_MOC_LITERAL(751, 15),  // "on_crop_clicked"
        QT_MOC_LITERAL(767, 17),  // "on_cutout_clicked"
        QT_MOC_LITERAL(785, 24)   // "on_open_cutout_triggered"
    },
    "ModellingWindow",
    "on_stamp_placed",
    "",
    "glm::vec3",
    "center",
    "SculptBrushShape",
    "shape_type",
    "glm::vec4",
    "params",
    "f32",
    "smoothness",
    "SculptBrushOp",
    "op",
    "material_slot",
    "looseness",
    "on_save_triggered",
    "on_load_triggered",
    "on_undo_triggered",
    "on_redo_triggered",
    "on_stroke_ended",
    "stamp_count",
    "on_shape_changed",
    "index",
    "on_op_changed",
    "on_smoothness_changed",
    "value",
    "on_size_changed",
    "on_looseness_changed",
    "on_stretch_x_changed",
    "on_stretch_y_changed",
    "on_stretch_z_changed",
    "on_finish_changed",
    "on_pick_material_colour_clicked",
    "on_fleck_changed",
    "on_fleck_strength_changed",
    "on_clone_mode_toggled",
    "checked",
    "on_set_clone_source_clicked",
    "on_clone_source_missing",
    "on_spray_mode_toggled",
    "on_spray_opacity_changed",
    "on_move_mode_toggled",
    "on_move_source_missing",
    "on_delete_edit_clicked",
    "on_move_edit_clicked",
    "on_edit_moved",
    "new_center",
    "on_crop_clicked",
    "on_cutout_clicked",
    "on_open_cutout_triggered"
};
#undef QT_MOC_LITERAL
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_ModellingWindow[] = {

 // content:
      10,       // revision
       0,       // classname
       0,    0, // classinfo
      31,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
       1,    7,  200,    2, 0x08,    1 /* Private */,
      15,    0,  215,    2, 0x08,    9 /* Private */,
      16,    0,  216,    2, 0x08,   10 /* Private */,
      17,    0,  217,    2, 0x08,   11 /* Private */,
      18,    0,  218,    2, 0x08,   12 /* Private */,
      19,    1,  219,    2, 0x08,   13 /* Private */,
      21,    1,  222,    2, 0x08,   15 /* Private */,
      23,    1,  225,    2, 0x08,   17 /* Private */,
      24,    1,  228,    2, 0x08,   19 /* Private */,
      26,    1,  231,    2, 0x08,   21 /* Private */,
      27,    1,  234,    2, 0x08,   23 /* Private */,
      28,    1,  237,    2, 0x08,   25 /* Private */,
      29,    1,  240,    2, 0x08,   27 /* Private */,
      30,    1,  243,    2, 0x08,   29 /* Private */,
      31,    1,  246,    2, 0x08,   31 /* Private */,
      32,    0,  249,    2, 0x08,   33 /* Private */,
      33,    1,  250,    2, 0x08,   34 /* Private */,
      34,    1,  253,    2, 0x08,   36 /* Private */,
      35,    1,  256,    2, 0x08,   38 /* Private */,
      37,    0,  259,    2, 0x08,   40 /* Private */,
      38,    0,  260,    2, 0x08,   41 /* Private */,
      39,    1,  261,    2, 0x08,   42 /* Private */,
      40,    1,  264,    2, 0x08,   44 /* Private */,
      41,    1,  267,    2, 0x08,   46 /* Private */,
      42,    0,  270,    2, 0x08,   48 /* Private */,
      43,    0,  271,    2, 0x08,   49 /* Private */,
      44,    0,  272,    2, 0x08,   50 /* Private */,
      45,    2,  273,    2, 0x08,   51 /* Private */,
      47,    0,  278,    2, 0x08,   54 /* Private */,
      48,    0,  279,    2, 0x08,   55 /* Private */,
      49,    0,  280,    2, 0x08,   56 /* Private */,

 // slots: parameters
    QMetaType::Void, 0x80000000 | 3, 0x80000000 | 5, 0x80000000 | 7, 0x80000000 | 9, 0x80000000 | 11, QMetaType::Int, 0x80000000 | 9,    4,    6,    8,   10,   12,   13,   14,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int,   20,
    QMetaType::Void, QMetaType::Int,   22,
    QMetaType::Void, QMetaType::Int,   22,
    QMetaType::Void, QMetaType::Double,   25,
    QMetaType::Void, QMetaType::Double,   25,
    QMetaType::Void, QMetaType::Double,   25,
    QMetaType::Void, QMetaType::Double,   25,
    QMetaType::Void, QMetaType::Double,   25,
    QMetaType::Void, QMetaType::Double,   25,
    QMetaType::Void, QMetaType::Int,   22,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int,   22,
    QMetaType::Void, QMetaType::Double,   25,
    QMetaType::Void, QMetaType::Bool,   36,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,   36,
    QMetaType::Void, QMetaType::Double,   25,
    QMetaType::Void, QMetaType::Bool,   36,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 3,   22,   46,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

Q_CONSTINIT const QMetaObject ModellingWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_meta_stringdata_ModellingWindow.offsetsAndSizes,
    qt_meta_data_ModellingWindow,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_ModellingWindow_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<ModellingWindow, std::true_type>,
        // method 'on_stamp_placed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<glm::vec3, std::false_type>,
        QtPrivate::TypeAndForceComplete<SculptBrushShape, std::false_type>,
        QtPrivate::TypeAndForceComplete<glm::vec4, std::false_type>,
        QtPrivate::TypeAndForceComplete<f32, std::false_type>,
        QtPrivate::TypeAndForceComplete<SculptBrushOp, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<f32, std::false_type>,
        // method 'on_save_triggered'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_load_triggered'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_undo_triggered'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_redo_triggered'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_stroke_ended'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'on_shape_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'on_op_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'on_smoothness_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'on_size_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'on_looseness_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'on_stretch_x_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'on_stretch_y_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'on_stretch_z_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'on_finish_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'on_pick_material_colour_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_fleck_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'on_fleck_strength_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'on_clone_mode_toggled'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'on_set_clone_source_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_clone_source_missing'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_spray_mode_toggled'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'on_spray_opacity_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'on_move_mode_toggled'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'on_move_source_missing'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_delete_edit_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_move_edit_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_edit_moved'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<glm::vec3, std::false_type>,
        // method 'on_crop_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_cutout_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_open_cutout_triggered'
        QtPrivate::TypeAndForceComplete<void, std::false_type>
    >,
    nullptr
} };

void ModellingWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ModellingWindow *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->on_stamp_placed((*reinterpret_cast< std::add_pointer_t<glm::vec3>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<SculptBrushShape>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<glm::vec4>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<f32>>(_a[4])),(*reinterpret_cast< std::add_pointer_t<SculptBrushOp>>(_a[5])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[6])),(*reinterpret_cast< std::add_pointer_t<f32>>(_a[7]))); break;
        case 1: _t->on_save_triggered(); break;
        case 2: _t->on_load_triggered(); break;
        case 3: _t->on_undo_triggered(); break;
        case 4: _t->on_redo_triggered(); break;
        case 5: _t->on_stroke_ended((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 6: _t->on_shape_changed((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 7: _t->on_op_changed((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 8: _t->on_smoothness_changed((*reinterpret_cast< std::add_pointer_t<double>>(_a[1]))); break;
        case 9: _t->on_size_changed((*reinterpret_cast< std::add_pointer_t<double>>(_a[1]))); break;
        case 10: _t->on_looseness_changed((*reinterpret_cast< std::add_pointer_t<double>>(_a[1]))); break;
        case 11: _t->on_stretch_x_changed((*reinterpret_cast< std::add_pointer_t<double>>(_a[1]))); break;
        case 12: _t->on_stretch_y_changed((*reinterpret_cast< std::add_pointer_t<double>>(_a[1]))); break;
        case 13: _t->on_stretch_z_changed((*reinterpret_cast< std::add_pointer_t<double>>(_a[1]))); break;
        case 14: _t->on_finish_changed((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 15: _t->on_pick_material_colour_clicked(); break;
        case 16: _t->on_fleck_changed((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 17: _t->on_fleck_strength_changed((*reinterpret_cast< std::add_pointer_t<double>>(_a[1]))); break;
        case 18: _t->on_clone_mode_toggled((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 19: _t->on_set_clone_source_clicked(); break;
        case 20: _t->on_clone_source_missing(); break;
        case 21: _t->on_spray_mode_toggled((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 22: _t->on_spray_opacity_changed((*reinterpret_cast< std::add_pointer_t<double>>(_a[1]))); break;
        case 23: _t->on_move_mode_toggled((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 24: _t->on_move_source_missing(); break;
        case 25: _t->on_delete_edit_clicked(); break;
        case 26: _t->on_move_edit_clicked(); break;
        case 27: _t->on_edit_moved((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<glm::vec3>>(_a[2]))); break;
        case 28: _t->on_crop_clicked(); break;
        case 29: _t->on_cutout_clicked(); break;
        case 30: _t->on_open_cutout_triggered(); break;
        default: ;
        }
    }
}

const QMetaObject *ModellingWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ModellingWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ModellingWindow.stringdata0))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int ModellingWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 31)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 31;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 31)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 31;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
