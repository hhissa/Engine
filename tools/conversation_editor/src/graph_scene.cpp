#include "graph_scene.h"
#include "connection_item.h"
#include "question_node.h"

#include <QColor>
#include <QGraphicsPathItem>
#include <QGraphicsSceneMouseEvent>
#include <QPainterPath>
#include <QPen>
#include <QTransform>

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace {
constexpr qreal kHorizontalSpacing = 280.0;
// Generous enough to comfortably fit a question plus a handful of answer
// lines without the next row overlapping it, now that a node's height
// grows with its content (see QuestionNode::content_height()) instead of
// being fixed -- a very long answer can still overlap the row below; nodes
// stay freely draggable afterward for exactly that case (see
// load_from_conversation()'s own comment).
constexpr qreal kVerticalSpacing = 220.0;

// How many leaves (nodes with no follow-ups of their own count as 1) live
// in question's subtree -- used by place() below to give every node a
// horizontal slice of screen space proportional to how much of the tree
// hangs off it, the standard "leaf-count-weighted" tree layout.
int leaf_count(const ConversationQuestion &question) {
  if (question.follow_ups.empty()) {
    return 1;
  }
  int total = 0;
  for (const ConversationQuestion &child : question.follow_ups) {
    total += leaf_count(child);
  }
  return total;
}

// Places question's node centered within the horizontal slice
// [x, x + leaf_count(question) * kHorizontalSpacing) at depth y, connects
// it to parent_node (if any), then lays out its own follow-ups left to
// right immediately below, each given a slice proportional to its own
// leaf_count(). Simple and good enough to be a readable starting point --
// nodes stay freely draggable afterward.
QuestionNode *place(GraphScene &scene, const ConversationQuestion &question,
                   qreal x, qreal y, QuestionNode *parent_node) {
  qreal width = static_cast<qreal>(leaf_count(question)) * kHorizontalSpacing;
  qreal center_x = x + width * 0.5 - QuestionNode::kWidth * 0.5;

  QuestionNode *node = scene.add_node(QPointF(center_x, y));
  node->set_question(QString::fromStdString(question.text));
  QStringList lines;
  lines.reserve(static_cast<int>(question.answer_lines.size()));
  for (const std::string &line : question.answer_lines) {
    lines << QString::fromStdString(line);
  }
  node->set_answer_lines(lines);

  if (parent_node) {
    scene.connect_nodes(parent_node, QuestionNode::Side::Bottom, node,
                       QuestionNode::Side::Top);
  }

  qreal child_x = x;
  for (const ConversationQuestion &child : question.follow_ups) {
    place(scene, child, child_x, y + kVerticalSpacing, node);
    child_x += static_cast<qreal>(leaf_count(child)) * kHorizontalSpacing;
  }
  return node;
}

// Inverse of place(): walks node's outgoing connections (its follow-ups)
// to build a ConversationQuestion tree. Called once per incoming edge into
// a node (see GraphScene::to_conversation()) -- a merge point (more than
// one parent, see GraphScene's class comment) is therefore serialized
// once per parent, each call independently rebuilding a full, identical
// copy of its subtree, which is exactly the duplication the file format's
// tree shape needs to represent something that, in the live editor, is
// only ever authored once.
ConversationQuestion to_question(const GraphScene &scene, QuestionNode *node) {
  ConversationQuestion question;
  question.text = node->question().toStdString();
  for (const QString &line : node->answer_lines()) {
    question.answer_lines.push_back(line.toStdString());
  }
  // A node's follow-ups are exactly the nodes it has an outgoing
  // connection to -- collected in the scene's own node order for a
  // deterministic (if otherwise arbitrary) file order.
  for (const ConnectionItem *connection : scene.connections()) {
    if (connection->source() == node) {
      question.follow_ups.push_back(to_question(scene, connection->target()));
    }
  }
  return question;
}
} // namespace

GraphScene::GraphScene(QObject *parent) : QGraphicsScene(parent) {
  connect(this, &QGraphicsScene::selectionChanged, this,
         &GraphScene::on_selection_changed);
}

GraphScene::~GraphScene() {
  clear_graph(); // see the declaration's comment for why this can't just
                 // be left to QGraphicsScene's own destructor
}

QuestionNode *GraphScene::add_node(QPointF pos) {
  auto *node = new QuestionNode();
  node->setPos(pos);
  addItem(node);
  nodes_.push_back(node);
  connect(node, &QuestionNode::moved, this, &GraphScene::on_node_moved);
  return node;
}

void GraphScene::detach_connection(ConnectionItem *connection) {
  connection->source()->unmark_side_connected(connection->source_side());
  connection->target()->unmark_side_connected(connection->target_side());
  connections_.erase(std::remove(connections_.begin(), connections_.end(), connection),
                     connections_.end());
}

void GraphScene::remove_node(QuestionNode *node) {
  // Remove every connection touching it first -- deleting the node while a
  // ConnectionItem still references it would leave that item holding a
  // dangling QuestionNode*.
  for (auto it = connections_.begin(); it != connections_.end();) {
    if ((*it)->source() == node || (*it)->target() == node) {
      ConnectionItem *connection = *it;
      connection->source()->unmark_side_connected(connection->source_side());
      connection->target()->unmark_side_connected(connection->target_side());
      delete connection; // also removes it from the scene (QGraphicsScene
                        // owns items added via addItem(); deleting one
                        // detaches it)
      it = connections_.erase(it);
    } else {
      ++it;
    }
  }

  nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), node), nodes_.end());
  delete node;
}

void GraphScene::remove_connection(ConnectionItem *connection) {
  detach_connection(connection);
  delete connection;
}

bool GraphScene::has_incoming_connection(QuestionNode *node) const {
  for (ConnectionItem *connection : connections_) {
    if (connection->target() == node) {
      return true;
    }
  }
  return false;
}

void GraphScene::clear_graph() {
  clear(); // QGraphicsScene::clear() deletes every item it owns
  nodes_.clear();
  connections_.clear();
  pending_source_ = nullptr;
  pending_line_ = nullptr;
}

void GraphScene::load_from_conversation(const Conversation &conversation) {
  clear_graph();
  qreal x = 0.0;
  for (const ConversationQuestion &question : conversation.questions) {
    place(*this, question, x, 0.0, nullptr);
    x += static_cast<qreal>(leaf_count(question)) * kHorizontalSpacing;
  }
}

Conversation GraphScene::to_conversation() const {
  Conversation conversation;
  for (QuestionNode *node : nodes_) {
    if (!has_incoming_connection(node)) {
      conversation.questions.push_back(to_question(*this, node));
    }
  }
  return conversation;
}

void GraphScene::connect_nodes(QuestionNode *source, QuestionNode::Side source_side,
                               QuestionNode *target, QuestionNode::Side target_side) {
  auto *connection = new ConnectionItem(source, source_side, target, target_side);
  addItem(connection);
  connections_.push_back(connection);
  source->mark_side_connected(source_side);
  target->mark_side_connected(target_side);
}

bool GraphScene::would_create_cycle(QuestionNode *source, QuestionNode *target) const {
  // Adding source -> target creates a cycle iff source is already
  // reachable from target by following existing outgoing connections --
  // i.e. target can already, directly or indirectly, lead back around to
  // source, which the new edge would then close into a loop. A plain
  // reachability search (BFS/DFS, order doesn't matter) from target, not
  // just a single-parent chain walk: with merge points allowed (see the
  // class comment), a node can have more than one incoming connection, so
  // there's no single "the" parent chain to walk anymore.
  std::vector<QuestionNode *> to_visit{target};
  std::unordered_set<QuestionNode *> visited;
  while (!to_visit.empty()) {
    QuestionNode *current = to_visit.back();
    to_visit.pop_back();
    if (current == source) {
      return true;
    }
    if (!visited.insert(current).second) {
      continue; // already explored -- avoids revisiting through another merge point
    }
    for (ConnectionItem *connection : connections_) {
      if (connection->source() == current) {
        to_visit.push_back(connection->target());
      }
    }
  }
  return false;
}

void GraphScene::mousePressEvent(QGraphicsSceneMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    QGraphicsItem *item = itemAt(event->scenePos(), QTransform());
    if (auto *node = qgraphicsitem_cast<QuestionNode *>(item)) {
      QPointF local = node->mapFromScene(event->scenePos());
      if (std::optional<QuestionNode::Side> side = node->side_at(local)) {
        pending_source_ = node;
        pending_source_side_ = *side;
        auto *line = new QGraphicsPathItem();
        line->setPen(QPen(QColor(120, 190, 255), 2.0, Qt::DashLine));
        line->setZValue(2.0);
        addItem(line);
        pending_line_ = line;
        return; // consumed -- don't let the base class start a node-body drag too
      }
    }
  }
  QGraphicsScene::mousePressEvent(event);
}

void GraphScene::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
  if (pending_source_) {
    // The start point is fixed to pending_source_side_ for the whole drag
    // (connections snap to a specific side's midpoint, not a continuously
    // sliding point) -- only the free end, following the cursor, moves.
    QPainterPath path(pending_source_->side_scene_pos(pending_source_side_));
    path.lineTo(event->scenePos());
    pending_line_->setPath(path);
    return;
  }
  QGraphicsScene::mouseMoveEvent(event);
}

void GraphScene::mouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
  if (pending_source_) {
    // Delete the rubber-band line *before* hit-testing for a drop target --
    // it's drawn above every node (zValue 2.0 vs. 1.0) and its path ends
    // exactly at the cursor, i.e. exactly where the drop is happening, so
    // leaving it in the scene during itemAt() below risks it shadowing the
    // very node being dropped onto.
    delete pending_line_;
    pending_line_ = nullptr;

    QGraphicsItem *item = itemAt(event->scenePos(), QTransform());
    auto *target = qgraphicsitem_cast<QuestionNode *>(item);

    if (target == pending_source_) {
      // Dropped back onto the very node the drag started from -- treat
      // this as a cancelled drag (no self-connections, no spawn).
      pending_source_ = nullptr;
      return;
    }

    bool spawned = false;
    if (!target) {
      // Dropped on empty canvas -- spawn a fresh question node right there
      // and connect straight into it instead of just discarding the drag,
      // the usual "drag a link into empty space to create the next node"
      // node-editor convention. Centered on the drop point rather than
      // top-left-anchored there (add_node()'s own convention), since a
      // spawn point should feel like "the node appeared here", not "the
      // node's corner landed here".
      QPointF spawn_pos =
          event->scenePos() - QPointF(QuestionNode::kWidth * 0.5, QuestionNode::kMinHeight * 0.5);
      target = add_node(spawn_pos);
      spawned = true;
    }

    // A node can now have more than one incoming connection -- several
    // questions merging into the same next one (see the class comment) --
    // so an existing incoming connection on target is left alone; the new
    // one is simply added alongside it. The only thing still guarded
    // against is a literal duplicate of the exact same source -> target
    // edge (dragging the same connection twice would otherwise duplicate
    // that follow-up in the exported file for no reason) and a cycle.
    bool duplicate = std::any_of(
        connections_.begin(), connections_.end(), [&](ConnectionItem *connection) {
          return connection->source() == pending_source_ && connection->target() == target;
        });

    if (!duplicate && !would_create_cycle(pending_source_, target)) {
      // Snap to whichever of target's sides is closest -- to the drop
      // point for an existing node (the drop doesn't need to land exactly
      // on a side handle), or to wherever pending_source_'s own side
      // actually is for a freshly spawned one (its center *is* the drop
      // point, so "nearest to the drop point" would be degenerate there).
      QuestionNode::Side target_side =
          spawned ? target->nearest_side_to(pending_source_->side_scene_pos(pending_source_side_))
                  : target->nearest_side_to(event->scenePos());
      connect_nodes(pending_source_, pending_source_side_, target, target_side);

      if (spawned) {
        // Select it immediately so MainWindow's side panel is ready for
        // the question text to be typed in right away.
        clearSelection();
        target->setSelected(true);
      }
    } else if (spawned) {
      // Unreachable in practice (a brand-new node can't already be an
      // ancestor of pending_source_), but if it somehow were, don't leave
      // an orphaned, unconnected node behind.
      remove_node(target);
    }

    pending_source_ = nullptr;
    return;
  }
  QGraphicsScene::mouseReleaseEvent(event);
}

void GraphScene::on_selection_changed() {
  QList<QGraphicsItem *> selected = selectedItems();
  QuestionNode *single = nullptr;
  if (selected.size() == 1) {
    single = qgraphicsitem_cast<QuestionNode *>(selected.first());
  }
  emit node_selected(single);
}

void GraphScene::on_node_moved() {
  auto *node = qobject_cast<QuestionNode *>(sender());
  if (!node) {
    return;
  }
  for (ConnectionItem *connection : connections_) {
    if (connection->source() == node || connection->target() == node) {
      connection->update_path();
    }
  }
}
