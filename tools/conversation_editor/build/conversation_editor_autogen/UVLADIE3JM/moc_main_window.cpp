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
struct qt_meta_stringdata_GraphView_t {
    uint offsetsAndSizes[2];
    char stringdata0[10];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_GraphView_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_GraphView_t qt_meta_stringdata_GraphView = {
    {
        QT_MOC_LITERAL(0, 9)   // "GraphView"
    },
    "GraphView"
};
#undef QT_MOC_LITERAL
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_GraphView[] = {

 // content:
      10,       // revision
       0,       // classname
       0,    0, // classinfo
       0,    0, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

       0        // eod
};

Q_CONSTINIT const QMetaObject GraphView::staticMetaObject = { {
    QMetaObject::SuperData::link<QGraphicsView::staticMetaObject>(),
    qt_meta_stringdata_GraphView.offsetsAndSizes,
    qt_meta_data_GraphView,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_GraphView_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<GraphView, std::true_type>
    >,
    nullptr
} };

void GraphView::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    (void)_o;
    (void)_id;
    (void)_c;
    (void)_a;
}

const QMetaObject *GraphView::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *GraphView::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_GraphView.stringdata0))
        return static_cast<void*>(this);
    return QGraphicsView::qt_metacast(_clname);
}

int GraphView::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QGraphicsView::qt_metacall(_c, _id, _a);
    return _id;
}
namespace {
struct qt_meta_stringdata_ConversationEditorWindow_t {
    uint offsetsAndSizes[28];
    char stringdata0[25];
    char stringdata1[24];
    char stringdata2[1];
    char stringdata3[27];
    char stringdata4[15];
    char stringdata5[16];
    char stringdata6[16];
    char stringdata7[17];
    char stringdata8[14];
    char stringdata9[5];
    char stringdata10[25];
    char stringdata11[24];
    char stringdata12[27];
    char stringdata13[30];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_ConversationEditorWindow_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_ConversationEditorWindow_t qt_meta_stringdata_ConversationEditorWindow = {
    {
        QT_MOC_LITERAL(0, 24),  // "ConversationEditorWindow"
        QT_MOC_LITERAL(25, 23),  // "on_add_question_clicked"
        QT_MOC_LITERAL(49, 0),  // ""
        QT_MOC_LITERAL(50, 26),  // "on_delete_selected_clicked"
        QT_MOC_LITERAL(77, 14),  // "on_new_clicked"
        QT_MOC_LITERAL(92, 15),  // "on_load_clicked"
        QT_MOC_LITERAL(108, 15),  // "on_save_clicked"
        QT_MOC_LITERAL(124, 16),  // "on_node_selected"
        QT_MOC_LITERAL(141, 13),  // "QuestionNode*"
        QT_MOC_LITERAL(155, 4),  // "node"
        QT_MOC_LITERAL(160, 24),  // "on_question_text_changed"
        QT_MOC_LITERAL(185, 23),  // "on_answer_lines_changed"
        QT_MOC_LITERAL(209, 26),  // "on_add_answer_line_clicked"
        QT_MOC_LITERAL(236, 29)   // "on_remove_answer_line_clicked"
    },
    "ConversationEditorWindow",
    "on_add_question_clicked",
    "",
    "on_delete_selected_clicked",
    "on_new_clicked",
    "on_load_clicked",
    "on_save_clicked",
    "on_node_selected",
    "QuestionNode*",
    "node",
    "on_question_text_changed",
    "on_answer_lines_changed",
    "on_add_answer_line_clicked",
    "on_remove_answer_line_clicked"
};
#undef QT_MOC_LITERAL
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_ConversationEditorWindow[] = {

 // content:
      10,       // revision
       0,       // classname
       0,    0, // classinfo
      10,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
       1,    0,   74,    2, 0x08,    1 /* Private */,
       3,    0,   75,    2, 0x08,    2 /* Private */,
       4,    0,   76,    2, 0x08,    3 /* Private */,
       5,    0,   77,    2, 0x08,    4 /* Private */,
       6,    0,   78,    2, 0x08,    5 /* Private */,
       7,    1,   79,    2, 0x08,    6 /* Private */,
      10,    0,   82,    2, 0x08,    8 /* Private */,
      11,    0,   83,    2, 0x08,    9 /* Private */,
      12,    0,   84,    2, 0x08,   10 /* Private */,
      13,    0,   85,    2, 0x08,   11 /* Private */,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 8,    9,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

Q_CONSTINIT const QMetaObject ConversationEditorWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_meta_stringdata_ConversationEditorWindow.offsetsAndSizes,
    qt_meta_data_ConversationEditorWindow,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_ConversationEditorWindow_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<ConversationEditorWindow, std::true_type>,
        // method 'on_add_question_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_delete_selected_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_new_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_load_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_save_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_node_selected'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QuestionNode *, std::false_type>,
        // method 'on_question_text_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_answer_lines_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_add_answer_line_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_remove_answer_line_clicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>
    >,
    nullptr
} };

void ConversationEditorWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ConversationEditorWindow *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->on_add_question_clicked(); break;
        case 1: _t->on_delete_selected_clicked(); break;
        case 2: _t->on_new_clicked(); break;
        case 3: _t->on_load_clicked(); break;
        case 4: _t->on_save_clicked(); break;
        case 5: _t->on_node_selected((*reinterpret_cast< std::add_pointer_t<QuestionNode*>>(_a[1]))); break;
        case 6: _t->on_question_text_changed(); break;
        case 7: _t->on_answer_lines_changed(); break;
        case 8: _t->on_add_answer_line_clicked(); break;
        case 9: _t->on_remove_answer_line_clicked(); break;
        default: ;
        }
    }
}

const QMetaObject *ConversationEditorWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ConversationEditorWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ConversationEditorWindow.stringdata0))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int ConversationEditorWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 10)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 10;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 10)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 10;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
