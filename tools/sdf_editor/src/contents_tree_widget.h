#pragma once
#include <QTreeWidget>

// A QTreeWidget specialized for the Primitives tab's "Scene Contents" tree:
// top-level items are layers, child items are the primitives within them
// (see SdfEditorWindow::refresh_contents_list()). Multi-select is enabled
// (QAbstractItemView::ExtendedSelection) -- SdfEditorWindow uses whichever
// primitive rows are selected to drive a (possibly grouped) gizmo
// selection in the 3D view (see SceneViewport::set_selection()).
//
// Adds two drag-and-drop moves, picked apart by what is being dragged:
//
//   * a primitive row (or the whole current multi-selection of primitive
//     rows) dropped onto a layer row -- or onto one of that layer's own
//     primitive children, which resolves to the same layer -- is a
//     *reparent*: it moves into that layer.
//   * a layer row (or a multi-selection of only layer rows) dropped
//     anywhere is a *reorder* of the top level, honouring the drop
//     indicator (above/below the row under the cursor, or the end of the
//     list when dropped into empty space). Layer order is authored data,
//     not presentation: the renderer folds layers in list order, so a
//     Subtract that runs before a Union is a different scene.
//
// Deliberately does NOT use QTreeWidget's own built-in InternalMove drop
// handling (dropEvent() below never calls the base implementation): that
// default logic reparents/reorders purely by drop *position*
// (above/below/on an item), which would let a primitive become a bare
// top-level item (no layer) or a layer become some primitive's child --
// neither makes sense for this tree's fixed two-level shape.
// SdfEditorWindow listens for primitives_reparented()/layers_reordered()
// to write the tree's new structure back into scene_ (see
// SdfEditorWindow::sync_layers_from_tree(), which reads both the top-level
// order and each layer's children) -- this class only ever rearranges its
// own QTreeWidgetItems, it never touches an SdfScene itself.
class ContentsTreeWidget : public QTreeWidget {
  Q_OBJECT

public:
  explicit ContentsTreeWidget(QWidget *parent = nullptr);

signals:
  // Fired after a drop actually moved at least one primitive item under a
  // different layer item than it started under.
  void primitives_reparented();

  // Fired after a drop actually changed the order of the top-level layer
  // items.
  void layers_reordered();

protected:
  void dropEvent(QDropEvent *event) override;

private:
  // dropEvent()'s layer-drag half: rearranges the top level so that every
  // selected layer item sits at the drop position, keeping their relative
  // order. `hit` is the item under the cursor (null when dropped into
  // empty space). Returns true if the order actually changed.
  bool reorder_layers(QTreeWidgetItem *hit);
};
