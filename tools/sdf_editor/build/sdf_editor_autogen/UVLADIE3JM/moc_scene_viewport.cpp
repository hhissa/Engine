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
    uint offsetsAndSizes[20];
    char stringdata0[14];
    char stringdata1[17];
    char stringdata2[1];
    char stringdata3[12];
    char stringdata4[22];
    char stringdata5[10];
    char stringdata6[9];
    char stringdata7[9];
    char stringdata8[7];
    char stringdata9[5];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_SceneViewport_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_SceneViewport_t qt_meta_stringdata_SceneViewport = {
    {
        QT_MOC_LITERAL(0, 13),  // "SceneViewport"
        QT_MOC_LITERAL(14, 16),  // "primitive_picked"
        QT_MOC_LITERAL(31, 0),  // ""
        QT_MOC_LITERAL(32, 11),  // "layer_index"
        QT_MOC_LITERAL(44, 21),  // "primitive_transformed"
        QT_MOC_LITERAL(66, 9),  // "glm::vec3"
        QT_MOC_LITERAL(76, 8),  // "position"
        QT_MOC_LITERAL(85, 8),  // "rotation"
        QT_MOC_LITERAL(94, 6),  // "params"
        QT_MOC_LITERAL(101, 4)   // "tick"
    },
    "SceneViewport",
    "primitive_picked",
    "",
    "layer_index",
    "primitive_transformed",
    "glm::vec3",
    "position",
    "rotation",
    "params",
    "tick"
};
#undef QT_MOC_LITERAL
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_SceneViewport[] = {

 // content:
      10,       // revision
       0,       // classname
       0,    0, // classinfo
       3,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    1,   32,    2, 0x06,    1 /* Public */,
       4,    4,   35,    2, 0x06,    3 /* Public */,

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
       9,    0,   44,    2, 0x08,    8 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::Int,    3,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 5, 0x80000000 | 5, 0x80000000 | 5,    3,    6,    7,    8,

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
        // method 'primitive_picked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'primitive_transformed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<glm::vec3, std::false_type>,
        QtPrivate::TypeAndForceComplete<glm::vec3, std::false_type>,
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
        case 0: _t->primitive_picked((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 1: _t->primitive_transformed((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<glm::vec3>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<glm::vec3>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<glm::vec3>>(_a[4]))); break;
        case 2: _t->tick(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (SceneViewport::*)(int );
            if (_t _q_method = &SceneViewport::primitive_picked; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (SceneViewport::*)(int , glm::vec3 , glm::vec3 , glm::vec3 );
            if (_t _q_method = &SceneViewport::primitive_transformed; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 1;
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
        if (_id < 3)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 3;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 3)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 3;
    }
    return _id;
}

// SIGNAL 0
void SceneViewport::primitive_picked(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void SceneViewport::primitive_transformed(int _t1, glm::vec3 _t2, glm::vec3 _t3, glm::vec3 _t4)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
