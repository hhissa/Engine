#include "contents_tree_widget.h"

#include <QDropEvent>

ContentsTreeWidget::ContentsTreeWidget(QWidget *parent) : QTreeWidget(parent) {
  setSelectionMode(QAbstractItemView::ExtendedSelection);
  setDragEnabled(true);
  setAcceptDrops(true);
  setDropIndicatorShown(true);
  // Only load-bearing insofar as it enables drag/drop at all -- the actual
  // move logic is entirely our own (see dropEvent() below), never Qt's
  // default InternalMove handling.
  setDragDropMode(QAbstractItemView::InternalMove);
}

void ContentsTreeWidget::dropEvent(QDropEvent *event) {
  QTreeWidgetItem *hit = itemAt(event->position().toPoint());
  // A primitive item's parent is its layer; a layer item has no parent and
  // is its own target. Anything else (dropped on empty space) has no valid
  // target at all.
  QTreeWidgetItem *target_layer =
      hit && hit->parent() ? hit->parent() : hit;
  if (!target_layer || target_layer->parent()) {
    event->ignore();
    return;
  }

  bool moved_any = false;
  for (QTreeWidgetItem *item : selectedItems()) {
    QTreeWidgetItem *old_parent = item->parent();
    if (!old_parent || old_parent == target_layer) {
      continue; // a layer item itself, or already under this layer
    }
    old_parent->removeChild(item);
    target_layer->addChild(item);
    moved_any = true;
  }
  target_layer->setExpanded(true);
  event->acceptProposedAction();

  if (moved_any) {
    emit primitives_reparented();
  }
}
