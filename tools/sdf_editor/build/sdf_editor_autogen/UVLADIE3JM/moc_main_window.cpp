/****************************************************************************
** Meta object code from reading C++ file 'main_window.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/main_window.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'main_window.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.1. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN15SdfEditorWindowE_t {};
} // unnamed namespace

template <> constexpr inline auto SdfEditorWindow::qt_create_metaobjectdata<qt_meta_tag_ZN15SdfEditorWindowE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SdfEditorWindow",
        "on_add_clicked",
        "",
        "on_remove_clicked",
        "on_new_layer_clicked",
        "on_copy_layer_clicked",
        "on_paste_layer_clicked",
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
        "on_splat_visibility_toggled",
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
        "on_repetition_mode_changed",
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

    QtMocHelpers::UintData qt_methods {
        // Slot 'on_add_clicked'
        QtMocHelpers::SlotData<void()>(1, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_remove_clicked'
        QtMocHelpers::SlotData<void()>(3, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_new_layer_clicked'
        QtMocHelpers::SlotData<void()>(4, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_copy_layer_clicked'
        QtMocHelpers::SlotData<void()>(5, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_paste_layer_clicked'
        QtMocHelpers::SlotData<void()>(6, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_colour_clicked'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_emissive_colour_clicked'
        QtMocHelpers::SlotData<void()>(8, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_texture_clicked'
        QtMocHelpers::SlotData<void()>(9, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_clear_texture_clicked'
        QtMocHelpers::SlotData<void()>(10, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_bump_map_clicked'
        QtMocHelpers::SlotData<void()>(11, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_clear_bump_map_clicked'
        QtMocHelpers::SlotData<void()>(12, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_save_clicked'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_load_clicked'
        QtMocHelpers::SlotData<void()>(14, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_type_selection_changed'
        QtMocHelpers::SlotData<void()>(15, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_move_mode_clicked'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_rotate_mode_clicked'
        QtMocHelpers::SlotData<void()>(17, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_show_grid_toggled'
        QtMocHelpers::SlotData<void(bool)>(18, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 19 },
        }}),
        // Slot 'on_splat_visibility_toggled'
        QtMocHelpers::SlotData<void(bool)>(20, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 19 },
        }}),
        // Slot 'on_viewport_selection_changed'
        QtMocHelpers::SlotData<void(std::vector<PrimitiveRef>)>(21, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 22, 23 },
        }}),
        // Slot 'on_viewport_primitives_transformed'
        QtMocHelpers::SlotData<void(std::vector<GizmoTransformResult>)>(24, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 25, 26 },
        }}),
        // Slot 'on_contents_tree_selection_changed'
        QtMocHelpers::SlotData<void()>(27, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_primitives_reparented'
        QtMocHelpers::SlotData<void()>(28, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_live_edit_changed'
        QtMocHelpers::SlotData<void()>(29, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_param_expr_changed'
        QtMocHelpers::SlotData<void()>(30, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_repetition_mode_changed'
        QtMocHelpers::SlotData<void()>(31, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_light_type_changed'
        QtMocHelpers::SlotData<void()>(32, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_add_light_clicked'
        QtMocHelpers::SlotData<void()>(33, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_remove_light_clicked'
        QtMocHelpers::SlotData<void()>(34, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_light_colour_clicked'
        QtMocHelpers::SlotData<void()>(35, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_lights_list_selection_changed'
        QtMocHelpers::SlotData<void()>(36, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_light_field_changed'
        QtMocHelpers::SlotData<void()>(37, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_ambient_changed'
        QtMocHelpers::SlotData<void()>(38, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_volumetric_type_changed'
        QtMocHelpers::SlotData<void()>(39, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_add_volumetric_clicked'
        QtMocHelpers::SlotData<void()>(40, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_remove_volumetric_clicked'
        QtMocHelpers::SlotData<void()>(41, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_volumetric_colour_clicked'
        QtMocHelpers::SlotData<void()>(42, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_volumetric_texture_clicked'
        QtMocHelpers::SlotData<void()>(43, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_clear_volumetric_texture_clicked'
        QtMocHelpers::SlotData<void()>(44, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_volumetrics_list_selection_changed'
        QtMocHelpers::SlotData<void()>(45, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_volumetric_field_changed'
        QtMocHelpers::SlotData<void()>(46, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SdfEditorWindow, qt_meta_tag_ZN15SdfEditorWindowE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SdfEditorWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15SdfEditorWindowE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15SdfEditorWindowE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN15SdfEditorWindowE_t>.metaTypes,
    nullptr
} };

void SdfEditorWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SdfEditorWindow *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->on_add_clicked(); break;
        case 1: _t->on_remove_clicked(); break;
        case 2: _t->on_new_layer_clicked(); break;
        case 3: _t->on_copy_layer_clicked(); break;
        case 4: _t->on_paste_layer_clicked(); break;
        case 5: _t->on_pick_colour_clicked(); break;
        case 6: _t->on_pick_emissive_colour_clicked(); break;
        case 7: _t->on_pick_texture_clicked(); break;
        case 8: _t->on_clear_texture_clicked(); break;
        case 9: _t->on_pick_bump_map_clicked(); break;
        case 10: _t->on_clear_bump_map_clicked(); break;
        case 11: _t->on_save_clicked(); break;
        case 12: _t->on_load_clicked(); break;
        case 13: _t->on_type_selection_changed(); break;
        case 14: _t->on_move_mode_clicked(); break;
        case 15: _t->on_rotate_mode_clicked(); break;
        case 16: _t->on_show_grid_toggled((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 17: _t->on_splat_visibility_toggled((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 18: _t->on_viewport_selection_changed((*reinterpret_cast<std::add_pointer_t<std::vector<PrimitiveRef>>>(_a[1]))); break;
        case 19: _t->on_viewport_primitives_transformed((*reinterpret_cast<std::add_pointer_t<std::vector<GizmoTransformResult>>>(_a[1]))); break;
        case 20: _t->on_contents_tree_selection_changed(); break;
        case 21: _t->on_primitives_reparented(); break;
        case 22: _t->on_live_edit_changed(); break;
        case 23: _t->on_param_expr_changed(); break;
        case 24: _t->on_repetition_mode_changed(); break;
        case 25: _t->on_light_type_changed(); break;
        case 26: _t->on_add_light_clicked(); break;
        case 27: _t->on_remove_light_clicked(); break;
        case 28: _t->on_pick_light_colour_clicked(); break;
        case 29: _t->on_lights_list_selection_changed(); break;
        case 30: _t->on_light_field_changed(); break;
        case 31: _t->on_ambient_changed(); break;
        case 32: _t->on_volumetric_type_changed(); break;
        case 33: _t->on_add_volumetric_clicked(); break;
        case 34: _t->on_remove_volumetric_clicked(); break;
        case 35: _t->on_pick_volumetric_colour_clicked(); break;
        case 36: _t->on_pick_volumetric_texture_clicked(); break;
        case 37: _t->on_clear_volumetric_texture_clicked(); break;
        case 38: _t->on_volumetrics_list_selection_changed(); break;
        case 39: _t->on_volumetric_field_changed(); break;
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
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15SdfEditorWindowE_t>.strings))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int SdfEditorWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 40)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 40;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 40)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 40;
    }
    return _id;
}
QT_WARNING_POP
