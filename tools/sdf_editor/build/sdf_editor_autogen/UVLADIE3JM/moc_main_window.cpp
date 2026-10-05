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
        "on_copy_primitives_clicked",
        "on_paste_primitives_clicked",
        "on_pick_colour_clicked",
        "on_pick_emissive_colour_clicked",
        "on_pick_absorption_colour_clicked",
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
        "on_gizmo_drag_started",
        "PrimitiveRef",
        "primitive",
        "on_gizmo_drag_moved",
        "GizmoTransformResult",
        "transform",
        "on_gizmo_drag_ended",
        "renderer_primitive_name",
        "std::string",
        "ref",
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
        "on_pick_skybox_clicked",
        "on_clear_skybox_clicked",
        "apply_scene_skybox",
        "on_volumetric_type_changed",
        "on_add_volumetric_clicked",
        "on_remove_volumetric_clicked",
        "on_pick_volumetric_colour_clicked",
        "on_materials_list_selection_changed",
        "on_rename_material_clicked",
        "on_duplicate_material_clicked",
        "on_delete_material_clicked",
        "on_assign_material_clicked",
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
        // Slot 'on_copy_primitives_clicked'
        QtMocHelpers::SlotData<void()>(5, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_paste_primitives_clicked'
        QtMocHelpers::SlotData<void()>(6, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_colour_clicked'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_emissive_colour_clicked'
        QtMocHelpers::SlotData<void()>(8, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_absorption_colour_clicked'
        QtMocHelpers::SlotData<void()>(9, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_texture_clicked'
        QtMocHelpers::SlotData<void()>(10, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_clear_texture_clicked'
        QtMocHelpers::SlotData<void()>(11, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_bump_map_clicked'
        QtMocHelpers::SlotData<void()>(12, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_clear_bump_map_clicked'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_save_clicked'
        QtMocHelpers::SlotData<void()>(14, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_load_clicked'
        QtMocHelpers::SlotData<void()>(15, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_type_selection_changed'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_move_mode_clicked'
        QtMocHelpers::SlotData<void()>(17, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_rotate_mode_clicked'
        QtMocHelpers::SlotData<void()>(18, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_show_grid_toggled'
        QtMocHelpers::SlotData<void(bool)>(19, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 20 },
        }}),
        // Slot 'on_splat_visibility_toggled'
        QtMocHelpers::SlotData<void(bool)>(21, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 20 },
        }}),
        // Slot 'on_viewport_selection_changed'
        QtMocHelpers::SlotData<void(std::vector<PrimitiveRef>)>(22, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 23, 24 },
        }}),
        // Slot 'on_viewport_primitives_transformed'
        QtMocHelpers::SlotData<void(std::vector<GizmoTransformResult>)>(25, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 26, 27 },
        }}),
        // Slot 'on_gizmo_drag_started'
        QtMocHelpers::SlotData<void(PrimitiveRef)>(28, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 29, 30 },
        }}),
        // Slot 'on_gizmo_drag_moved'
        QtMocHelpers::SlotData<void(GizmoTransformResult)>(31, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 32, 33 },
        }}),
        // Slot 'on_gizmo_drag_ended'
        QtMocHelpers::SlotData<void()>(34, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'renderer_primitive_name'
        QtMocHelpers::SlotData<std::string(PrimitiveRef) const>(35, 2, QMC::AccessPrivate, 0x80000000 | 36, {{
            { 0x80000000 | 29, 37 },
        }}),
        // Slot 'on_contents_tree_selection_changed'
        QtMocHelpers::SlotData<void()>(38, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_primitives_reparented'
        QtMocHelpers::SlotData<void()>(39, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_live_edit_changed'
        QtMocHelpers::SlotData<void()>(40, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_param_expr_changed'
        QtMocHelpers::SlotData<void()>(41, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_repetition_mode_changed'
        QtMocHelpers::SlotData<void()>(42, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_light_type_changed'
        QtMocHelpers::SlotData<void()>(43, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_add_light_clicked'
        QtMocHelpers::SlotData<void()>(44, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_remove_light_clicked'
        QtMocHelpers::SlotData<void()>(45, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_light_colour_clicked'
        QtMocHelpers::SlotData<void()>(46, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_lights_list_selection_changed'
        QtMocHelpers::SlotData<void()>(47, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_light_field_changed'
        QtMocHelpers::SlotData<void()>(48, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_ambient_changed'
        QtMocHelpers::SlotData<void()>(49, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_skybox_clicked'
        QtMocHelpers::SlotData<void()>(50, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_clear_skybox_clicked'
        QtMocHelpers::SlotData<void()>(51, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'apply_scene_skybox'
        QtMocHelpers::SlotData<void()>(52, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_volumetric_type_changed'
        QtMocHelpers::SlotData<void()>(53, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_add_volumetric_clicked'
        QtMocHelpers::SlotData<void()>(54, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_remove_volumetric_clicked'
        QtMocHelpers::SlotData<void()>(55, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_volumetric_colour_clicked'
        QtMocHelpers::SlotData<void()>(56, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_materials_list_selection_changed'
        QtMocHelpers::SlotData<void()>(57, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_rename_material_clicked'
        QtMocHelpers::SlotData<void()>(58, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_duplicate_material_clicked'
        QtMocHelpers::SlotData<void()>(59, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_delete_material_clicked'
        QtMocHelpers::SlotData<void()>(60, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_assign_material_clicked'
        QtMocHelpers::SlotData<void()>(61, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_volumetric_texture_clicked'
        QtMocHelpers::SlotData<void()>(62, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_clear_volumetric_texture_clicked'
        QtMocHelpers::SlotData<void()>(63, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_volumetrics_list_selection_changed'
        QtMocHelpers::SlotData<void()>(64, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_volumetric_field_changed'
        QtMocHelpers::SlotData<void()>(65, 2, QMC::AccessPrivate, QMetaType::Void),
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
        case 3: _t->on_copy_primitives_clicked(); break;
        case 4: _t->on_paste_primitives_clicked(); break;
        case 5: _t->on_pick_colour_clicked(); break;
        case 6: _t->on_pick_emissive_colour_clicked(); break;
        case 7: _t->on_pick_absorption_colour_clicked(); break;
        case 8: _t->on_pick_texture_clicked(); break;
        case 9: _t->on_clear_texture_clicked(); break;
        case 10: _t->on_pick_bump_map_clicked(); break;
        case 11: _t->on_clear_bump_map_clicked(); break;
        case 12: _t->on_save_clicked(); break;
        case 13: _t->on_load_clicked(); break;
        case 14: _t->on_type_selection_changed(); break;
        case 15: _t->on_move_mode_clicked(); break;
        case 16: _t->on_rotate_mode_clicked(); break;
        case 17: _t->on_show_grid_toggled((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 18: _t->on_splat_visibility_toggled((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 19: _t->on_viewport_selection_changed((*reinterpret_cast<std::add_pointer_t<std::vector<PrimitiveRef>>>(_a[1]))); break;
        case 20: _t->on_viewport_primitives_transformed((*reinterpret_cast<std::add_pointer_t<std::vector<GizmoTransformResult>>>(_a[1]))); break;
        case 21: _t->on_gizmo_drag_started((*reinterpret_cast<std::add_pointer_t<PrimitiveRef>>(_a[1]))); break;
        case 22: _t->on_gizmo_drag_moved((*reinterpret_cast<std::add_pointer_t<GizmoTransformResult>>(_a[1]))); break;
        case 23: _t->on_gizmo_drag_ended(); break;
        case 24: { std::string _r = _t->renderer_primitive_name((*reinterpret_cast<std::add_pointer_t<PrimitiveRef>>(_a[1])));
            if (_a[0]) *reinterpret_cast<std::string*>(_a[0]) = std::move(_r); }  break;
        case 25: _t->on_contents_tree_selection_changed(); break;
        case 26: _t->on_primitives_reparented(); break;
        case 27: _t->on_live_edit_changed(); break;
        case 28: _t->on_param_expr_changed(); break;
        case 29: _t->on_repetition_mode_changed(); break;
        case 30: _t->on_light_type_changed(); break;
        case 31: _t->on_add_light_clicked(); break;
        case 32: _t->on_remove_light_clicked(); break;
        case 33: _t->on_pick_light_colour_clicked(); break;
        case 34: _t->on_lights_list_selection_changed(); break;
        case 35: _t->on_light_field_changed(); break;
        case 36: _t->on_ambient_changed(); break;
        case 37: _t->on_pick_skybox_clicked(); break;
        case 38: _t->on_clear_skybox_clicked(); break;
        case 39: _t->apply_scene_skybox(); break;
        case 40: _t->on_volumetric_type_changed(); break;
        case 41: _t->on_add_volumetric_clicked(); break;
        case 42: _t->on_remove_volumetric_clicked(); break;
        case 43: _t->on_pick_volumetric_colour_clicked(); break;
        case 44: _t->on_materials_list_selection_changed(); break;
        case 45: _t->on_rename_material_clicked(); break;
        case 46: _t->on_duplicate_material_clicked(); break;
        case 47: _t->on_delete_material_clicked(); break;
        case 48: _t->on_assign_material_clicked(); break;
        case 49: _t->on_pick_volumetric_texture_clicked(); break;
        case 50: _t->on_clear_volumetric_texture_clicked(); break;
        case 51: _t->on_volumetrics_list_selection_changed(); break;
        case 52: _t->on_volumetric_field_changed(); break;
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
        if (_id < 53)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 53;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 53)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 53;
    }
    return _id;
}
QT_WARNING_POP
