#include "graph_scene.h"
#include "connection_item.h"
#include "question_node.h"

#include <core/logger.h>

#include <QColor>
#include <QGraphicsPathItem>
#include <QGraphicsSceneMouseEvent>
#include <QPainterPath>
#include <QPen>
#include <QTransform>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {
constexpr qreal kHorizontalSpacing = 280.0;
// Gap left below a node's actual, content-dependent height (see
// QuestionNode::content_height()) before its follow-up row starts --
// place() below adds this to the node's real height rather than using a
// fixed row-to-row spacing, so a long answer can no longer push a node
// into the row underneath it. Matches kHorizontalSpacing's own slack over
// QuestionNode::kWidth (280 - 220 = 60) for a consistent visual rhythm.
constexpr qreal kVerticalMargin = 60.0;

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
//
// question may itself be a question_ref= stand-in (see
// resources/conversation.h's ConversationQuestion::is_reference) -- in
// that case the *referenced* definition's content (looked up in
// definitions) is what actually gets placed. Either way, a question with a
// shared_id is only ever built into a node once: shared_nodes remembers
// that node (keyed by shared_id) across the whole load_from_conversation()
// call, so every other attachment point -- whether it's a question_ref=
// stub or the id='s own natural position in the tree -- just adds another
// incoming connection into that same node instead of creating a
// duplicate, reconstructing the merge point the file's id=/question_ref=
// pair was written to represent (see GraphScene's own class comment).
//
// built_nodes records every node built here keyed by the ORIGINAL
// (non-reference) ConversationQuestion it came from -- both the plain case
// and the question_ref= case key off the same definition pointer (`source`
// above), so a loop_to= pointing at a question reached via a ref still
// resolves to the right node. Used by connect_loop_edges() below, run as a
// second pass once every node has been placed (a loop_to= target might not
// have a node yet -- or might not even have been visited yet -- at the
// point its referencing question is placed, since loop_to= is deliberately
// allowed to point anywhere, including forward or at an ancestor).
QuestionNode *place(GraphScene &scene, const ConversationQuestion &question,
                   qreal x, qreal y, QuestionNode *parent_node,
                   const std::unordered_map<std::string, const ConversationQuestion *> &definitions,
                   std::unordered_map<std::string, QuestionNode *> &shared_nodes,
                   std::unordered_map<const ConversationQuestion *, QuestionNode *> &built_nodes) {
  const ConversationQuestion *source = &question;
  if (question.is_reference) {
    std::string id = question.shared_id.value_or(std::string());
    auto it = definitions.find(id);
    if (it == definitions.end()) {
      KWARN("Conversation editor: question_ref='{}' has no matching id= -- "
           "skipping.",
           id);
      return nullptr;
    }
    source = it->second;
  }

  QuestionNode *node = nullptr;
  if (source->shared_id) {
    auto it = shared_nodes.find(*source->shared_id);
    if (it != shared_nodes.end()) {
      node = it->second;
    }
  }

  if (!node) {
    qreal width = static_cast<qreal>(leaf_count(*source)) * kHorizontalSpacing;
    qreal center_x = x + width * 0.5 - QuestionNode::kWidth * 0.5;

    node = scene.add_node(QPointF(center_x, y));
    node->set_question(QString::fromStdString(source->text));
    QStringList lines;
    lines.reserve(static_cast<int>(source->answer_lines.size()));
    for (const std::string &line : source->answer_lines) {
      lines << QString::fromStdString(line);
    }
    node->set_answer_lines(lines);
    node->set_tag(source->tag ? QString::fromStdString(*source->tag) : QString());
    {
      QStringList requires_flags;
      for (const std::string &flag : source->requires_flags) {
        requires_flags << QString::fromStdString(flag);
      }
      node->set_requires_flags(requires_flags);
      QStringList requires_not_flags;
      for (const std::string &flag : source->requires_not_flags) {
        requires_not_flags << QString::fromStdString(flag);
      }
      node->set_requires_not_flags(requires_not_flags);
      QStringList sets_flags;
      for (const std::string &flag : source->sets_flags) {
        sets_flags << QString::fromStdString(flag);
      }
      node->set_sets_flags(sets_flags);
    }
    node->set_is_ending(source->is_ending);
    {
      QStringList ending_lines;
      for (const std::string &line : source->ending_lines) {
        ending_lines << QString::fromStdString(line);
      }
      node->set_ending_lines(ending_lines);
    }
    built_nodes[source] = node;

    if (source->shared_id) {
      shared_nodes[*source->shared_id] = node;
    }

    qreal child_x = x;
    qreal child_y = y + node->content_height() + kVerticalMargin;
    for (const ConversationQuestion &child : source->follow_ups) {
      place(scene, child, child_x, child_y, node, definitions, shared_nodes,
           built_nodes);
      child_x += static_cast<qreal>(leaf_count(child)) * kHorizontalSpacing;
    }
  }

  if (parent_node) {
    scene.connect_nodes(parent_node, QuestionNode::Side::Bottom, node,
                       QuestionNode::Side::Top);
  }
  return node;
}

// Second pass after every node is placed: walks question (and, recursively,
// its follow_ups -- but never into a question_ref= stub, which carries no
// loop_target of its own, see resources/conversation.h) wiring a Loop
// connection (see ConnectionItem::Kind) for every loop_to= found, from its
// own node (via built_nodes) to its target's node (via shared_nodes, keyed
// by the same id= a question_ref= would use). Logs a warning and skips a
// loop_to= whose target/own node can't be found rather than failing the
// whole load.
void connect_loop_edges(GraphScene &scene, const ConversationQuestion &question,
                       const std::unordered_map<std::string, QuestionNode *> &shared_nodes,
                       const std::unordered_map<const ConversationQuestion *, QuestionNode *> &built_nodes) {
  if (question.is_reference) {
    return;
  }
  if (question.loop_target) {
    auto node_it = built_nodes.find(&question);
    auto target_it = shared_nodes.find(*question.loop_target);
    if (node_it == built_nodes.end() || target_it == shared_nodes.end()) {
      KWARN("Conversation editor: loop_to='{}' has no matching id= or no "
           "node of its own -- skipping.",
           *question.loop_target);
    } else {
      QuestionNode *node = node_it->second;
      QuestionNode *target = target_it->second;
      scene.connect_nodes(node, node->nearest_side_to(target->scenePos()), target,
                         target->nearest_side_to(node->scenePos()),
                         ConnectionItem::Kind::Loop);
    }
  }
  for (const ConversationQuestion &child : question.follow_ups) {
    connect_loop_edges(scene, child, shared_nodes, built_nodes);
  }
}

// Inverse of place(): walks node's outgoing connections (its follow-ups)
// to build a ConversationQuestion tree. Called once per incoming edge into
// a node (see GraphScene::to_conversation()) -- a merge point (more than
// one parent, see GraphScene's class comment) would therefore normally be
// serialized once per parent, each call independently rebuilding a full,
// identical copy of its subtree. shared_ids/serialized avoid that: a node
// listed in shared_ids gets its full content written out (with its
// shared_id attached, see resources/conversation.h's `id=`) only the
// *first* time this is called on it; serialized then remembers that, so
// every subsequent call for the same node returns a bare `question_ref=`
// stand-in instead of re-walking (and re-duplicating) its subtree.
ConversationQuestion to_question(const GraphScene &scene, QuestionNode *node,
                                 const std::unordered_map<QuestionNode *, std::string> &shared_ids,
                                 std::unordered_set<QuestionNode *> &serialized) {
  auto shared_it = shared_ids.find(node);
  bool is_shared = shared_it != shared_ids.end();
  if (is_shared && serialized.contains(node)) {
    ConversationQuestion ref;
    ref.shared_id = shared_it->second;
    ref.is_reference = true;
    return ref;
  }

  ConversationQuestion question;
  question.text = node->question().toStdString();
  for (const QString &line : node->answer_lines()) {
    question.answer_lines.push_back(line.toStdString());
  }
  if (!node->tag().isEmpty()) {
    question.tag = node->tag().toStdString();
  }
  for (const QString &flag : node->requires_flags()) {
    question.requires_flags.push_back(flag.toStdString());
  }
  for (const QString &flag : node->requires_not_flags()) {
    question.requires_not_flags.push_back(flag.toStdString());
  }
  for (const QString &flag : node->sets_flags()) {
    question.sets_flags.push_back(flag.toStdString());
  }
  question.is_ending = node->is_ending();
  for (const QString &line : node->ending_lines()) {
    question.ending_lines.push_back(line.toStdString());
  }
  if (is_shared) {
    question.shared_id = shared_it->second;
    serialized.insert(node);
  }
  // A node's follow-ups are exactly the nodes it has an outgoing FollowUp
  // connection to -- collected in the scene's own node order for a
  // deterministic (if otherwise arbitrary) file order. An outgoing Loop
  // connection (see ConnectionItem::Kind) doesn't contribute a follow-up
  // at all -- it becomes a loop_to= line on `question` itself instead (a
  // node has at most one; the first found wins if a stray graph somehow
  // has more).
  for (const ConnectionItem *connection : scene.connections()) {
    if (connection->source() != node) {
      continue;
    }
    if (connection->kind() == ConnectionItem::Kind::Loop) {
      if (!question.loop_target) {
        auto target_shared_it = shared_ids.find(connection->target());
        if (target_shared_it != shared_ids.end()) {
          question.loop_target = target_shared_it->second;
        }
      }
      continue;
    }
    question.follow_ups.push_back(
        to_question(scene, connection->target(), shared_ids, serialized));
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

QuestionNode *GraphScene::insert_node_on_connection(ConnectionItem *connection, QPointF pos) {
  QuestionNode *source = connection->source();
  QuestionNode *target = connection->target();
  QuestionNode::Side source_side = connection->source_side();
  QuestionNode::Side target_side = connection->target_side();
  ConnectionItem::Kind kind = connection->kind();

  // Capture the endpoints' scene-space attachment points before removing
  // connection -- used below to pick the new node's own two sides, and
  // still valid afterward since removing a connection doesn't move either
  // node.
  QPointF source_point = source->side_scene_pos(source_side);
  QPointF target_point = target->side_scene_pos(target_side);
  remove_connection(connection);

  // Centered on pos rather than top-left-anchored there, same convention
  // as mouseReleaseEvent()'s drag-to-empty-canvas spawn -- pos is where
  // the user actually clicked along the curve, so the new node should feel
  // like it appeared right there.
  QPointF spawn_pos =
      pos - QPointF(QuestionNode::kWidth * 0.5, QuestionNode::kMinHeight * 0.5);
  QuestionNode *node = add_node(spawn_pos);

  QuestionNode::Side node_side_to_source = node->nearest_side_to(source_point);
  QuestionNode::Side node_side_to_target = node->nearest_side_to(target_point);
  connect_nodes(source, source_side, node, node_side_to_source, kind);
  connect_nodes(node, node_side_to_target, target, target_side, kind);

  clearSelection();
  node->setSelected(true);
  return node;
}

bool GraphScene::has_incoming_connection(QuestionNode *node) const {
  for (ConnectionItem *connection : connections_) {
    if (connection->target() == node && connection->kind() == ConnectionItem::Kind::FollowUp) {
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
  // Built once up front so a question_ref= encountered before its id='s
  // own natural position in the tree (order in the file doesn't matter --
  // see resources/conversation.h) can still be resolved -- see place().
  std::unordered_map<std::string, const ConversationQuestion *> definitions =
      collect_shared_definitions(conversation);
  std::unordered_map<std::string, QuestionNode *> shared_nodes;
  std::unordered_map<const ConversationQuestion *, QuestionNode *> built_nodes;

  qreal x = 0.0;
  for (const ConversationQuestion &question : conversation.questions) {
    place(*this, question, x, 0.0, nullptr, definitions, shared_nodes, built_nodes);
    x += static_cast<qreal>(leaf_count(question)) * kHorizontalSpacing;
  }
  // Second pass -- every node now exists, so loop_to= targets (which may
  // point anywhere already authored, including forward or at an ancestor)
  // can all be resolved regardless of file order -- see connect_loop_edges().
  for (const ConversationQuestion &question : conversation.questions) {
    connect_loop_edges(*this, question, shared_nodes, built_nodes);
  }
}

Conversation GraphScene::to_conversation() const {
  // A node reached by more than one incoming FollowUp connection is a
  // merge point (see the class comment) -- give each one a fresh id so
  // to_question() can write its content out exactly once (as `id=`) and
  // stand in a bare `question_ref=` everywhere else, instead of
  // duplicating its whole subtree per incoming edge.
  std::unordered_map<QuestionNode *, int> incoming_counts;
  for (ConnectionItem *connection : connections_) {
    if (connection->kind() == ConnectionItem::Kind::FollowUp) {
      ++incoming_counts[connection->target()];
    }
  }
  std::unordered_map<QuestionNode *, std::string> shared_ids;
  int next_id = 1;
  for (QuestionNode *node : nodes_) {
    if (incoming_counts[node] > 1) {
      shared_ids[node] = "shared" + std::to_string(next_id++);
    }
  }
  // Any node targeted by a Loop connection also needs an id -- even one
  // with zero or one incoming FollowUp connections -- so the looping
  // question(s) can name it via `loop_to=`.
  for (ConnectionItem *connection : connections_) {
    if (connection->kind() == ConnectionItem::Kind::Loop &&
        !shared_ids.contains(connection->target())) {
      shared_ids[connection->target()] = "shared" + std::to_string(next_id++);
    }
  }

  Conversation conversation;
  std::unordered_set<QuestionNode *> serialized;
  for (QuestionNode *node : nodes_) {
    if (!has_incoming_connection(node)) {
      conversation.questions.push_back(to_question(*this, node, shared_ids, serialized));
    }
  }
  return conversation;
}

void GraphScene::connect_nodes(QuestionNode *source, QuestionNode::Side source_side,
                               QuestionNode *target, QuestionNode::Side target_side,
                               ConnectionItem::Kind kind) {
  auto *connection = new ConnectionItem(source, source_side, target, target_side, kind);
  addItem(connection);
  connections_.push_back(connection);
  source->mark_side_connected(source_side);
  target->mark_side_connected(target_side);
}

bool GraphScene::would_create_cycle(QuestionNode *source, QuestionNode *target) const {
  // Adding source -> target creates a cycle iff source is already
  // reachable from target by following existing outgoing FollowUp
  // connections -- i.e. target can already, directly or indirectly, lead
  // back around to source, which the new edge would then close into a
  // loop. A plain reachability search (BFS/DFS, order doesn't matter) from
  // target, not just a single-parent chain walk: with merge points allowed
  // (see the class comment), a node can have more than one incoming
  // connection, so there's no single "the" parent chain to walk anymore.
  // Loop connections are skipped -- see this function's own declaration
  // comment for why.
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
      if (connection->source() == current && connection->kind() == ConnectionItem::Kind::FollowUp) {
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

    // A node can now have more than one incoming FollowUp connection --
    // several questions merging into the same next one (see the class
    // comment) -- so an existing incoming connection on target is left
    // alone; the new one is simply added alongside it. A spawned target is
    // brand new, so it can never already be an ancestor of pending_source_
    // -- always a plain FollowUp. For an existing target, dropping onto
    // one that's already reachable from pending_source_ (i.e. would close
    // a cycle -- see would_create_cycle()) means the player is looping
    // back to an earlier question rather than continuing forward, so that
    // becomes a Loop connection (see ConnectionItem::Kind) instead of
    // being rejected. Either way, the only thing guarded against is a
    // literal duplicate of the exact same source -> target edge of the
    // same kind (dragging the same connection twice would otherwise
    // duplicate that follow-up/loop in the exported file for no reason).
    ConnectionItem::Kind kind = (!spawned && would_create_cycle(pending_source_, target))
                                    ? ConnectionItem::Kind::Loop
                                    : ConnectionItem::Kind::FollowUp;

    bool duplicate = std::any_of(
        connections_.begin(), connections_.end(), [&](ConnectionItem *connection) {
          return connection->source() == pending_source_ && connection->target() == target &&
                connection->kind() == kind;
        });

    if (!duplicate) {
      if (kind == ConnectionItem::Kind::Loop) {
        // A question can only loop to one place -- dragging a fresh Loop
        // connection from the same source replaces whichever one it
        // already had, rather than leaving both and letting to_question()
        // silently pick one.
        for (auto it = connections_.begin(); it != connections_.end();) {
          ConnectionItem *connection = *it;
          if (connection->source() == pending_source_ &&
              connection->kind() == ConnectionItem::Kind::Loop) {
            detach_connection(connection);
            delete connection;
            it = connections_.begin(); // detach_connection() re-walks connections_
          } else {
            ++it;
          }
        }
      }

      // Snap to whichever of target's sides is closest -- to the drop
      // point for an existing node (the drop doesn't need to land exactly
      // on a side handle), or to wherever pending_source_'s own side
      // actually is for a freshly spawned one (its center *is* the drop
      // point, so "nearest to the drop point" would be degenerate there).
      QuestionNode::Side target_side =
          spawned ? target->nearest_side_to(pending_source_->side_scene_pos(pending_source_side_))
                  : target->nearest_side_to(event->scenePos());
      connect_nodes(pending_source_, pending_source_side_, target, target_side, kind);

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
