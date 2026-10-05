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

  // What is being dragged decides which move this is. A layer row has no
  // parent, so there is nothing to reparent it into -- dragging layers is
  // a reorder of the top level instead. A mixed selection (layers and
  // primitives at once) has no single sensible reading, so it falls
  // through to the reparent path, which skips the layer rows in it.
  bool dragging_only_layers = !selectedItems().isEmpty();
  for (QTreeWidgetItem *item : selectedItems()) {
    if (item->parent()) {
      dragging_only_layers = false;
      break;
    }
  }
  if (dragging_only_layers) {
    bool moved = reorder_layers(hit);
    event->acceptProposedAction();
    if (moved) {
      emit layers_reordered();
    }
    return;
  }

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

bool ContentsTreeWidget::reorder_layers(QTreeWidgetItem *hit) {
  // Dropping onto one of a layer's primitives means that layer, same as
  // the reparent path resolves it.
  QTreeWidgetItem *target = hit && hit->parent() ? hit->parent() : hit;

  // Split the top level into the layers being dragged and the ones staying
  // put, both in their current order -- a multi-layer drag keeps the
  // dragged layers' relative order and lands them as one contiguous run.
  QList<QTreeWidgetItem *> selected = selectedItems();
  QList<QTreeWidgetItem *> dragged;
  QList<QTreeWidgetItem *> rest;
  for (int i = 0; i < topLevelItemCount(); ++i) {
    QTreeWidgetItem *item = topLevelItem(i);
    if (selected.contains(item)) {
      dragged.push_back(item);
    } else {
      rest.push_back(item);
    }
  }
  if (dragged.isEmpty() || rest.isEmpty()) {
    return false; // nothing to move, or nothing to move it past
  }

  // Where the run lands, as an index into `rest`. A drop onto the item
  // itself (rather than the gap above/below it) takes that item's place,
  // i.e. inserts before it -- so dragging a layer onto the one above it
  // moves it up by one, which is the whole point of the gesture.
  int insert_at = rest.size(); // no target row: dropped past the end
  if (target) {
    int target_index = rest.indexOf(target);
    if (target_index < 0) {
      return false; // dropped onto one of the layers being dragged
    }
    insert_at = dropIndicatorPosition() == QAbstractItemView::BelowItem
                    ? target_index + 1
                    : target_index;
  }

  QList<QTreeWidgetItem *> ordered = rest.mid(0, insert_at);
  ordered.append(dragged);
  ordered.append(rest.mid(insert_at));

  QList<QTreeWidgetItem *> current;
  for (int i = 0; i < topLevelItemCount(); ++i) {
    current.push_back(topLevelItem(i));
  }
  if (ordered == current) {
    return false;
  }

  // takeTopLevelItem() detaches an item with its children intact, so this
  // rebuild moves whole layers, primitives and all.
  while (topLevelItemCount() > 0) {
    takeTopLevelItem(0);
  }
  for (QTreeWidgetItem *item : ordered) {
    addTopLevelItem(item);
    item->setExpanded(true);
  }
  return true;
}
