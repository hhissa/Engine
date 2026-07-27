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
struct qt_meta_stringdata_SdfEditorWindow_t {
    uint offsetsAndSizes[86];
    char stringdata0[16];
    char stringdata1[15];
    char stringdata2[1];
    char stringdata3[18];
    char stringdata4[21];
    char stringdata5[23];
    char stringdata6[32];
    char stringdata7[24];
    char stringdata8[25];
    char stringdata9[25];
    char stringdata10[26];
    char stringdata11[16];
    char stringdata12[16];
    char stringdata13[26];
    char stringdata14[21];
    char stringdata15[23];
    char stringdata16[21];
    char stringdata17[8];
    char stringdata18[30];
    char stringdata19[26];
    char stringdata20[10];
    char stringdata21[35];
    char stringdata22[34];
    char stringdata23[8];
    char stringdata24[35];
    char stringdata25[25];
    char stringdata26[21];
    char stringdata27[22];
    char stringdata28[22];
    char stringdata29[21];
    char stringdata30[24];
    char stringdata31[29];
    char stringdata32[33];
    char stringdata33[23];
    char stringdata34[19];
    char stringdata35[27];
    char stringdata36[26];
    char stringdata37[29];
    char stringdata38[34];
    char stringdata39[35];
    char stringdata40[36];
    char stringdata41[38];
    char stringdata42[28];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_SdfEditorWindow_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_SdfEditorWindow_t qt_meta_stringdata_SdfEditorWindow = {
    {
        QT_MOC_LITERAL(0, 15),  // "SdfEditorWindow"
        QT_MOC_LITERAL(16, 14),  // "on_add_clicked"
        QT_MOC_LITERAL(31, 0),  // ""
        QT_MOC_LITERAL(32, 17),  // "on_remove_clicked"
        QT_MOC_LITERAL(50, 20),  // "on_new_layer_clicked"
        QT_MOC_LITERAL(71, 22),  // "on_pick_colour_clicked"
        QT_MOC_LITERAL(94, 31),  // "on_pick_emissive_colour_clicked"
        QT_MOC_LITERAL(126, 23),  // "on_pick_texture_clicked"
        QT_MOC_LITERAL(150, 24),  // "on_clear_texture_clicked"
        QT_MOC_LITERAL(175, 24),  // "on_pick_bump_map_clicked"
        QT_MOC_LITERAL(200, 25),  // "on_clear_bump_map_clicked"
        QT_MOC_LITERAL(226, 15),  // "on_save_clicked"
        QT_MOC_LITERAL(242, 15),  // "on_load_clicked"
        QT_MOC_LITERAL(258, 25),  // "on_type_selection_changed"
        QT_MOC_LITERAL(284, 20),  // "on_move_mode_clicked"
        QT_MOC_LITERAL(305, 22),  // "on_rotate_mode_clicked"
        QT_MOC_LITERAL(328, 20),  // "on_show_grid_toggled"
        QT_MOC_LITERAL(349, 7),  // "checked"
        QT_MOC_LITERAL(357, 29),  // "on_viewport_selection_changed"
        QT_MOC_LITERAL(387, 25),  // "std::vector<PrimitiveRef>"
        QT_MOC_LITERAL(413, 9),  // "selection"
        QT_MOC_LITERAL(423, 34),  // "on_viewport_primitives_transf..."
        QT_MOC_LITERAL(458, 33),  // "std::vector<GizmoTransformRes..."
        QT_MOC_LITERAL(492, 7),  // "results"
        QT_MOC_LITERAL(500, 34),  // "on_contents_tree_selection_ch..."
        QT_MOC_LITERAL(535, 24),  // "on_primitives_reparented"
        QT_MOC_LITERAL(560, 20),  // "on_live_edit_changed"
        QT_MOC_LITERAL(581, 21),  // "on_param_expr_changed"
        QT_MOC_LITERAL(603, 21),  // "on_light_type_changed"
        QT_MOC_LITERAL(625, 20),  // "on_add_light_clicked"
        QT_MOC_LITERAL(646, 23),  // "on_remove_light_clicked"
        QT_MOC_LITERAL(670, 28),  // "on_pick_light_colour_clicked"
        QT_MOC_LITERAL(699, 32),  // "on_lights_list_selection_changed"
        QT_MOC_LITERAL(732, 22),  // "on_light_field_changed"
        QT_MOC_LITERAL(755, 18),  // "on_ambient_changed"
        QT_MOC_LITERAL(774, 26),  // "on_volumetric_type_changed"
        QT_MOC_LITERAL(801, 25),  // "on_add_volumetric_clicked"
        QT_MOC_LITERAL(827, 28),  // "on_remove_volumetric_clicked"
        QT_MOC_LITERAL(856, 33),  // "on_pick_volumetric_colour_cli..."
        QT_MOC_LITERAL(890, 34),  // "on_pick_volumetric_texture_cl..."
        QT_MOC_LITERAL(925, 35),  // "on_clear_volumetric_texture_c..."
        QT_MOC_LITERAL(961, 37),  // "on_volumetrics_list_selection..."
        QT_MOC_LITERAL(999, 27)   // "on_volumetric_field_changed"
    },
    "SdfEditorWindow",
    "on_add_clicked",
    "",
    "on_remove_clicked",
    "on_new_layer_clicked",
    "on_pick_colour_clicked",
    "on_pick_emissive_colour_clicked",
    "on_pick_texture_clicked",
    "on_clear_texture_clicked",
    "on_pick_bump_map_clicked",
    "on_clear_bump_map_clicked",
    "on_save_clicked",
    "on_load_clicked",
    "on_type_selection_changed",
    "on_move_mode_clicked",
    "on_rotate_mode_clicked",
    "on_show_grid_toggled",
    "checked",
    "on_viewport_selection_changed",
    "std::vector<PrimitiveRef>",
    "selection",
    "on_viewport_primitives_transformed",
    "std::vector<GizmoTransformResult>",
    "results",
    "on_contents_tree_selection_changed",
    "on_primitives_reparented",
    "on_live_edit_changed",
    "on_param_expr_changed",
    "on_light_type_changed",
    "on_add_light_clicked",
    "on_remove_light_clicked",
    "on_pick_light_colour_clicked",
    "on_lights_list_selection_changed",
    "on_light_field_changed",
    "on_ambient_changed",
    "on_volumetric_type_changed",
    "on_add_volumetric_clicked",
    "on_remove_volumetric_clicked",
    "on_pick_volumetric_colour_clicked",
    "on_pick_volumetric_texture_clicked",
    "on_clear_volumetric_texture_clicked",
    "on_volumetrics_list_selection_changed",
    "on_volumetric_field_changed"
};
#undef QT_MOC_LITERAL
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_SdfEditorWindow[] = {

 // content:
      10,       // revision
       0,       // classname
       0,    0, // classinfo
      36,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
       1,    0,  230,    2, 0x08,    1 /* Private */,
       3,    0,  231,    2, 0x08,    2 /* Private */,
       4,    0,  232,    2, 0x08,    3 /* Private */,
       5,    0,  233,    2, 0x08,    4 /* Private */,
       6,    0,  234,    2, 0x08,    5 /* Private */,
       7,    0,  235,    2, 0x08,    6 /* Private */,
       8,    0,  236,    2, 0x08,    7 /* Private */,
       9,    0,  237,    2, 0x08,    8 /* Private */,
      10,    0,  238,    2, 0x08,    9 /* Private */,
      11,    0,  239,    2, 0x08,   10 /* Private */,
      12,    0,  240,    2, 0x08,   11 /* Private */,
      13,    0,  241,    2, 0x08,   12 /* Private */,
      14,    0,  242,    2, 0x08,   13 /* Private */,
      15,    0,  243,    2, 0x08,   14 /* Private */,
      16,    1,  244,    2, 0x08,   15 /* Private */,
      18,    1,  247,    2, 0x08,   17 /* Private */,
      21,    1,  250,    2, 0x08,   19 /* Private */,
      24,    0,  253,    2, 0x08,   21 /* Private */,
      25,    0,  254,    2, 0x08,   22 /* Private */,
      26,    0,  255,    2, 0x08,   23 /* Private */,
      27,    0,  256,    2, 0x08,   24 /* Private */,
      28,    0,  257,    2, 0x08,   25 /* Private */,
      29,    0,  258,    2, 0x08,   26 /* Private */,
      30,    0,  259,    2, 0x08,   27 /* Private */,
      31,    0,  260,    2, 0x08,   28 /* Private */,
      32,    0,  261,    2, 0x08,   29 /* Private */,
      33,    0,  262,    2, 0x08,   30 /* Private */,
      34,    0,  263,    2, 0x08,   31 /* Private */,
      35,    0,  264,    2, 0x08,   32 /* Private */,
      36,    0,  265,    2, 0x08,   33 /* Private */,
      37,    0,  266,    2, 0x08,   34 /* Private */,
      38,    0,  267,    2, 0x08,   35 /* Private */,
      39,    0,  268,    2, 0x08,   36 /* Private */,
      40,    0,  269,    2, 0x08,   37 /* Private */,
      41,    0,  270,    2, 0x08,   38 /* Private */,
      42,    0,  271,    2, 0x08,   39 /* Private */,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,   17,
    QMetaType::Void, 0x80000000 | 19,   20,
    QMetaType::Void, 0x80000000 | 22,   23,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

Q_CONSTINIT const QMetaObject SdfEditorWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_meta_stringdata_SdfEditorWindow.offsetsAndSizes,
    qt_meta_data_SdfEditorWindow,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_SdfEditorWindow_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<SdfEditorWindow, std::true_type>,
        // method 'on_add_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_remove_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_new_layer_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_pick_colour_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_pick_emissive_colour_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_pick_texture_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_clear_texture_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_pick_bump_map_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_clear_bump_map_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_save_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_load_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_type_selection_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_move_mode_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_rotate_mode_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_show_grid_toggled'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'on_viewport_selection_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<std::vector<PrimitiveRef>, std::false_type>,
        // method 'on_viewport_primitives_transformed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<std::vector<GizmoTransformResult>, std::false_type>,
        // method 'on_contents_tree_selection_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_primitives_reparented'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_live_edit_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_param_expr_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_light_type_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_add_light_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_remove_light_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_pick_light_colour_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_lights_list_selection_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_light_field_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_ambient_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_volumetric_type_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_add_volumetric_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_remove_volumetric_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_pick_volumetric_colour_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_pick_volumetric_texture_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_clear_volumetric_texture_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_volumetrics_list_selection_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_volumetric_field_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>
    >,
    nullptr
} };

void SdfEditorWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<SdfEditorWindow *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->on_add_clicked(); break;
        case 1: _t->on_remove_clicked(); break;
        case 2: _t->on_new_layer_clicked(); break;
        case 3: _t->on_pick_colour_clicked(); break;
        case 4: _t->on_pick_emissive_colour_clicked(); break;
        case 5: _t->on_pick_texture_clicked(); break;
        case 6: _t->on_clear_texture_clicked(); break;
        case 7: _t->on_pick_bump_map_clicked(); break;
        case 8: _t->on_clear_bump_map_clicked(); break;
        case 9: _t->on_save_clicked(); break;
        case 10: _t->on_load_clicked(); break;
        case 11: _t->on_type_selection_changed(); break;
        case 12: _t->on_move_mode_clicked(); break;
        case 13: _t->on_rotate_mode_clicked(); break;
        case 14: _t->on_show_grid_toggled((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 15: _t->on_viewport_selection_changed((*reinterpret_cast< std::add_pointer_t<std::vector<PrimitiveRef>>>(_a[1]))); break;
        case 16: _t->on_viewport_primitives_transformed((*reinterpret_cast< std::add_pointer_t<std::vector<GizmoTransformResult>>>(_a[1]))); break;
        case 17: _t->on_contents_tree_selection_changed(); break;
        case 18: _t->on_primitives_reparented(); break;
        case 19: _t->on_live_edit_changed(); break;
        case 20: _t->on_param_expr_changed(); break;
        case 21: _t->on_light_type_changed(); break;
        case 22: _t->on_add_light_clicked(); break;
        case 23: _t->on_remove_light_clicked(); break;
        case 24: _t->on_pick_light_colour_clicked(); break;
        case 25: _t->on_lights_list_selection_changed(); break;
        case 26: _t->on_light_field_changed(); break;
        case 27: _t->on_ambient_changed(); break;
        case 28: _t->on_volumetric_type_changed(); break;
        case 29: _t->on_add_volumetric_clicked(); break;
        case 30: _t->on_remove_volumetric_clicked(); break;
        case 31: _t->on_pick_volumetric_colour_clicked(); break;
        case 32: _t->on_pick_volumetric_texture_clicked(); break;
        case 33: _t->on_clear_volumetric_texture_clicked(); break;
        case 34: _t->on_volumetrics_list_selection_changed(); break;
        case 35: _t->on_volumetric_field_changed(); break;
        default: ;
        }
    }
}

const QMetaObject *SdfEditorWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SdfEditorWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_SdfEditorWindow.stringdata0))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int SdfEditorWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 36)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 36;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 36)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 36;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
