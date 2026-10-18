/****************************************************************************
** Meta object code from reading C++ file 'scene_viewport.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.4.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../src/scene_viewport.h"
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'scene_viewport.h' doesn't include <QObject>."
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
struct qt_meta_stringdata_SceneViewport_t {
    uint offsetsAndSizes[48];
    char stringdata0[14];
    char stringdata1[13];
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
    char stringdata15[13];
    char stringdata16[12];
    char stringdata17[22];
    char stringdata18[21];
    char stringdata19[20];
    char stringdata20[11];
    char stringdata21[6];
    char stringdata22[11];
    char stringdata23[5];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_SceneViewport_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_SceneViewport_t qt_meta_stringdata_SceneViewport = {
    {
        QT_MOC_LITERAL(0, 13),  // "SceneViewport"
        QT_MOC_LITERAL(14, 12),  // "stamp_placed"
        QT_MOC_LITERAL(27, 0),  // ""
        QT_MOC_LITERAL(28, 9),  // "glm::vec3"
        QT_MOC_LITERAL(38, 6),  // "center"
        QT_MOC_LITERAL(45, 16),  // "SculptBrushShape"
        QT_MOC_LITERAL(62, 10),  // "shape_type"
        QT_MOC_LITERAL(73, 9),  // "glm::vec4"
        QT_MOC_LITERAL(83, 6),  // "params"
        QT_MOC_LITERAL(90, 3),  // "f32"
        QT_MOC_LITERAL(94, 10),  // "smoothness"
        QT_MOC_LITERAL(105, 13),  // "SculptBrushOp"
        QT_MOC_LITERAL(119, 2),  // "op"
        QT_MOC_LITERAL(122, 13),  // "material_slot"
        QT_MOC_LITERAL(136, 9),  // "looseness"
        QT_MOC_LITERAL(146, 12),  // "stroke_ended"
        QT_MOC_LITERAL(159, 11),  // "stamp_count"
        QT_MOC_LITERAL(171, 21),  // "clone_source_captured"
        QT_MOC_LITERAL(193, 20),  // "clone_source_missing"
        QT_MOC_LITERAL(214, 19),  // "move_source_missing"
        QT_MOC_LITERAL(234, 10),  // "edit_moved"
        QT_MOC_LITERAL(245, 5),  // "index"
        QT_MOC_LITERAL(251, 10),  // "new_center"
        QT_MOC_LITERAL(262, 4)   // "tick"
    },
    "SceneViewport",
    "stamp_placed",
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
    "stroke_ended",
    "stamp_count",
    "clone_source_captured",
    "clone_source_missing",
    "move_source_missing",
    "edit_moved",
    "index",
    "new_center",
    "tick"
};
#undef QT_MOC_LITERAL
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_SceneViewport[] = {

 // content:
      10,       // revision
       0,       // classname
       0,    0, // classinfo
       7,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       6,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    7,   56,    2, 0x06,    1 /* Public */,
      15,    1,   71,    2, 0x06,    9 /* Public */,
      17,    0,   74,    2, 0x06,   11 /* Public */,
      18,    0,   75,    2, 0x06,   12 /* Public */,
      19,    0,   76,    2, 0x06,   13 /* Public */,
      20,    2,   77,    2, 0x06,   14 /* Public */,

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
      23,    0,   82,    2, 0x08,   17 /* Private */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3, 0x80000000 | 5, 0x80000000 | 7, 0x80000000 | 9, 0x80000000 | 11, QMetaType::Int, 0x80000000 | 9,    4,    6,    8,   10,   12,   13,   14,
    QMetaType::Void, QMetaType::Int,   16,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 3,   21,   22,

 // slots: parameters
    QMetaType::Void,

       0        // eod
};

Q_CONSTINIT const QMetaObject SceneViewport::staticMetaObject = { {
    QMetaObject::SuperData::link<QWindow::staticMetaObject>(),
    qt_meta_stringdata_SceneViewport.offsetsAndSizes,
    qt_meta_data_SceneViewport,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_SceneViewport_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<SceneViewport, std::true_type>,
        // method 'stamp_placed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<glm::vec3, std::false_type>,
        QtPrivate::TypeAndForceComplete<SculptBrushShape, std::false_type>,
        QtPrivate::TypeAndForceComplete<glm::vec4, std::false_type>,
        QtPrivate::TypeAndForceComplete<f32, std::false_type>,
        QtPrivate::TypeAndForceComplete<SculptBrushOp, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<f32, std::false_type>,
        // method 'stroke_ended'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'clone_source_captured'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'clone_source_missing'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'move_source_missing'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'edit_moved'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<glm::vec3, std::false_type>,
        // method 'tick'
        QtPrivate::TypeAndForceComplete<void, std::false_type>
    >,
    nullptr
} };

void SceneViewport::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<SceneViewport *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->stamp_placed((*reinterpret_cast< std::add_pointer_t<glm::vec3>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<SculptBrushShape>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<glm::vec4>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<f32>>(_a[4])),(*reinterpret_cast< std::add_pointer_t<SculptBrushOp>>(_a[5])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[6])),(*reinterpret_cast< std::add_pointer_t<f32>>(_a[7]))); break;
        case 1: _t->stroke_ended((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 2: _t->clone_source_captured(); break;
        case 3: _t->clone_source_missing(); break;
        case 4: _t->move_source_missing(); break;
        case 5: _t->edit_moved((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<glm::vec3>>(_a[2]))); break;
        case 6: _t->tick(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (SceneViewport::*)(glm::vec3 , SculptBrushShape , glm::vec4 , f32 , SculptBrushOp , int , f32 );
            if (_t _q_method = &SceneViewport::stamp_placed; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (SceneViewport::*)(int );
            if (_t _q_method = &SceneViewport::stroke_ended; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (SceneViewport::*)();
            if (_t _q_method = &SceneViewport::clone_source_captured; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (SceneViewport::*)();
            if (_t _q_method = &SceneViewport::clone_source_missing; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (SceneViewport::*)();
            if (_t _q_method = &SceneViewport::move_source_missing; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (SceneViewport::*)(int , glm::vec3 );
            if (_t _q_method = &SceneViewport::edit_moved; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 5;
                return;
            }
        }
    }
}

const QMetaObject *SceneViewport::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SceneViewport::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_SceneViewport.stringdata0))
        return static_cast<void*>(this);
    return QWindow::qt_metacast(_clname);
}

int SceneViewport::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 7)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 7)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 7;
    }
    return _id;
}

// SIGNAL 0
void SceneViewport::stamp_placed(glm::vec3 _t1, SculptBrushShape _t2, glm::vec4 _t3, f32 _t4, SculptBrushOp _t5, int _t6, f32 _t7)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t6))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t7))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void SceneViewport::stroke_ended(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void SceneViewport::clone_source_captured()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void SceneViewport::clone_source_missing()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void SceneViewport::move_source_missing()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void SceneViewport::edit_moved(int _t1, glm::vec3 _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
