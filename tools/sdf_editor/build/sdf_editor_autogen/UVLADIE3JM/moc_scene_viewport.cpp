/****************************************************************************
** Meta object code from reading C++ file 'scene_viewport.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/scene_viewport.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'scene_viewport.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN13SceneViewportE_t {};
} // unnamed namespace

template <> constexpr inline auto SceneViewport::qt_create_metaobjectdata<qt_meta_tag_ZN13SceneViewportE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SceneViewport",
        "selection_changed",
        "",
        "std::vector<PrimitiveRef>",
        "selection",
        "gizmo_drag_started",
        "PrimitiveRef",
        "primitive",
        "gizmo_drag_moved",
        "GizmoTransformResult",
        "transform",
        "gizmo_drag_ended",
        "primitives_transformed",
        "std::vector<GizmoTransformResult>",
        "results",
        "tick"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'selection_changed'
        QtMocHelpers::SignalData<void(std::vector<PrimitiveRef>)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Signal 'gizmo_drag_started'
        QtMocHelpers::SignalData<void(PrimitiveRef)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 6, 7 },
        }}),
        // Signal 'gizmo_drag_moved'
        QtMocHelpers::SignalData<void(GizmoTransformResult)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 9, 10 },
        }}),
        // Signal 'gizmo_drag_ended'
        QtMocHelpers::SignalData<void()>(11, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'primitives_transformed'
        QtMocHelpers::SignalData<void(std::vector<GizmoTransformResult>)>(12, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 13, 14 },
        }}),
        // Slot 'tick'
        QtMocHelpers::SlotData<void()>(15, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SceneViewport, qt_meta_tag_ZN13SceneViewportE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SceneViewport::staticMetaObject = { {
    QMetaObject::SuperData::link<QWindow::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13SceneViewportE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13SceneViewportE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN13SceneViewportE_t>.metaTypes,
    nullptr
} };

void SceneViewport::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SceneViewport *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->selection_changed((*reinterpret_cast<std::add_pointer_t<std::vector<PrimitiveRef>>>(_a[1]))); break;
        case 1: _t->gizmo_drag_started((*reinterpret_cast<std::add_pointer_t<PrimitiveRef>>(_a[1]))); break;
        case 2: _t->gizmo_drag_moved((*reinterpret_cast<std::add_pointer_t<GizmoTransformResult>>(_a[1]))); break;
        case 3: _t->gizmo_drag_ended(); break;
        case 4: _t->primitives_transformed((*reinterpret_cast<std::add_pointer_t<std::vector<GizmoTransformResult>>>(_a[1]))); break;
        case 5: _t->tick(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (SceneViewport::*)(std::vector<PrimitiveRef> )>(_a, &SceneViewport::selection_changed, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (SceneViewport::*)(PrimitiveRef )>(_a, &SceneViewport::gizmo_drag_started, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (SceneViewport::*)(GizmoTransformResult )>(_a, &SceneViewport::gizmo_drag_moved, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (SceneViewport::*)()>(_a, &SceneViewport::gizmo_drag_ended, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (SceneViewport::*)(std::vector<GizmoTransformResult> )>(_a, &SceneViewport::primitives_transformed, 4))
            return;
    }
}

const QMetaObject *SceneViewport::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SceneViewport::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13SceneViewportE_t>.strings))
        return static_cast<void*>(this);
    return QWindow::qt_metacast(_clname);
}

int SceneViewport::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 6)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 6;
    }
    return _id;
}

// SIGNAL 0
void SceneViewport::selection_changed(std::vector<PrimitiveRef> _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void SceneViewport::gizmo_drag_started(PrimitiveRef _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void SceneViewport::gizmo_drag_moved(GizmoTransformResult _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void SceneViewport::gizmo_drag_ended()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void SceneViewport::primitives_transformed(std::vector<GizmoTransformResult> _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}
QT_WARNING_POP
