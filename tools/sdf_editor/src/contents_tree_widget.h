#pragma once
#include <QTreeWidget>

// A QTreeWidget specialized for the Primitives tab's "Scene Contents" tree:
// top-level items are layers, child items are the primitives within them
// (see SdfEditorWindow::refresh_contents_list()). Multi-select is enabled
// (QAbstractItemView::ExtendedSelection) -- SdfEditorWindow uses whichever
// primitive rows are selected to drive a (possibly grouped) gizmo
// selection in the 3D view (see SceneViewport::set_selection()).
//
// Adds drag-and-drop reparenting: dropping a primitive row (or the whole
// current multi-selection of primitive rows) onto a layer row -- or onto
// one of that layer's own primitive children, which resolves to the same
// layer -- moves it there in the tree. Deliberately does NOT use
// QTreeWidget's own built-in InternalMove drop handling (dropEvent() below
// never calls the base implementation): that default logic reparents/
// reorders purely by drop *position* (above/below/on an item), which would
// let a primitive become a bare top-level item (no layer) or a layer
// become some primitive's child -- neither makes sense for this tree's
// fixed two-level shape. SdfEditorWindow listens for primitives_reparented()
// to write the tree's new structure back into scene_ (see
// SdfEditorWindow::sync_layers_from_tree()) -- this class only ever
// rearranges its own QTreeWidgetItems, it never touches an SdfScene itself.
class ContentsTreeWidget : public QTreeWidget {
  Q_OBJECT

public:
  explicit ContentsTreeWidget(QWidget *parent = nullptr);

signals:
  // Fired after a drop actually moved at least one primitive item under a
  // different layer item than it started under.
  void primitives_reparented();

protected:
  void dropEvent(QDropEvent *event) override;
};
