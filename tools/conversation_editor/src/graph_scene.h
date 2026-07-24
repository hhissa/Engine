#pragma once
#include "question_node.h"

#include <resources/conversation.h>

#include <QGraphicsScene>
#include <QPointF>

#include <vector>

class ConnectionItem;
class QGraphicsPathItem;

// Owns the conversation graph: QuestionNode items and the ConnectionItems
// between them. A connection is directed (source -> target) and means
// "target is a follow-up of source" -- see QuestionNode's own comment.
// Neither incoming nor outgoing connections are capped: a straight chain
// (Q1 -> Q2 -> Q3 -> Q4) is exactly as valid as a branching tree, and
// several different questions can all connect INTO the same node -- a
// merge point, letting a handful of different follow-up threads all
// converge back onto one shared next question once whichever one the
// player actually took is exhausted. A node with zero incoming
// connections is a top-level question; connecting FROM a node makes the
// target a follow-up of it, to any depth ("nodes without any connections
// exist at the top level, nodes connected to them are subquestions").
//
// The .conversation file format itself is still a plain tree (see
// engine/src/resources/conversation.h -- each question's follow_ups is a
// vector it owns outright, not a reference to something shared), so a
// merge point isn't written once and pointed at twice: to_conversation()
// walks every incoming edge into a merged node and re-serializes that
// node's *entire* subtree once per edge, producing one textually-identical
// copy under each parent. In play, that's indistinguishable from a real
// shared destination -- whichever branch the player actually took leads to
// its own copy of the same question -- it just costs some duplication in
// the saved file rather than in the editor, where you still only ever
// author and edit that question once.
//
// Connections are made by dragging from the midpoint of one of a node's
// four sides (see QuestionNode::side_at()) to another node, snapping to
// whichever of the target's sides is nearest the drop point (see
// QuestionNode::nearest_side_to()) -- this class overrides the mouse
// handlers to detect a side-press (rather than the ordinary QGraphicsItem
// body-drag Qt already handles for free, which still applies everywhere
// else on the box) and draws a temporary dashed rubber-band line while the
// drag is in progress.
class GraphScene : public QGraphicsScene {
  Q_OBJECT
public:
  explicit GraphScene(QObject *parent = nullptr);
  // Explicitly tears the graph down (see clear_graph()) rather than
  // leaving it to QGraphicsScene's own base destructor to delete every
  // item later: by that point GraphScene's own derived-class destructor
  // body has already finished, so a QuestionNode's moved() signal being
  // disconnected mid-deletion can find its receiver (`this`) no longer
  // "fully GraphScene" -- Qt catches this and asserts ("Called object is
  // not of the correct type (class destructor may have already run)").
  // Clearing here, while `this` is still intact, avoids that.
  ~GraphScene() override;

  // Adds a fresh node at scene position pos and returns it -- the scene
  // owns every item added to it, per normal QGraphicsScene rules (nothing
  // further for the caller, e.g. MainWindow's "Add Question" action, to
  // manage).
  QuestionNode *add_node(QPointF pos);

  // Removes node and every connection touching it (both incoming and
  // outgoing), un-marking the side each one attached to on whichever
  // *other* node it touched (see QuestionNode::unmark_side_connected()).
  // A node's follow-ups are NOT removed -- they simply lose this one
  // incoming connection, becoming top-level themselves unless a merge
  // point kept another one connected into them too (see the class
  // comment).
  void remove_node(QuestionNode *node);

  // Removes just this one connection (un-marking both endpoints' sides)
  // -- its target loses that one incoming connection, becoming top-level
  // unless it was a merge point with another parent still connected;
  // everything else about the graph is unaffected.
  void remove_connection(ConnectionItem *connection);

  // True if node has at least one incoming connection (i.e. it's a
  // follow-up of something, whether one parent or several merging into
  // it) -- false means node is top-level.
  bool has_incoming_connection(QuestionNode *node) const;

  // Clears every node/connection -- used by MainWindow's "New" action and
  // before a fresh load_from_conversation() call.
  void clear_graph();

  // Rebuilds the entire graph from a parsed Conversation (see
  // engine/src/resources/conversation.h), laying out nodes automatically
  // (a simple top-down tree layout: each root side by side, each node's
  // follow-ups stacked below it) -- any existing graph content is
  // discarded first (see clear_graph()). Nodes stay freely draggable
  // afterward; this layout is only meant to be a readable starting point.
  void load_from_conversation(const Conversation &conversation);

  // Converts the current graph into a Conversation tree: every node with
  // no incoming connection becomes a top-level ConversationQuestion, and
  // every node reachable by following outgoing connections becomes a
  // nested follow-up, recursively -- the exact inverse of
  // load_from_conversation() (modulo node layout, which isn't part of the
  // file format). A node with more than one incoming connection (a merge
  // point -- see the class comment) gets serialized once per incoming
  // edge, each a full, independent copy of that node's own subtree.
  Conversation to_conversation() const;

  const std::vector<QuestionNode *> &nodes() const { return nodes_; }
  const std::vector<ConnectionItem *> &connections() const { return connections_; }

  // Constructs and registers a ConnectionItem between the given sides of
  // source/target, marking both sides connected, without the cycle-check
  // an interactive drag applies (see mouseReleaseEvent()) -- used by
  // load_from_conversation(), which builds connections directly from
  // already-validated (by construction -- it's a tree on disk) file data.
  // Public so free helper functions building a graph programmatically
  // (see graph_scene.cpp's place()) can call it without needing to be
  // members; callers driving this from user interaction should go through
  // the normal drag gesture instead, which enforces no-cycles.
  void connect_nodes(QuestionNode *source, QuestionNode::Side source_side,
                     QuestionNode *target, QuestionNode::Side target_side);

signals:
  // Fired whenever the selection settles on exactly one node (nullptr if
  // zero or more than one are selected) -- MainWindow's side panel
  // listens for this to populate/clear its fields.
  void node_selected(QuestionNode *node);

protected:
  void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
  void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
  void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;

private slots:
  void on_selection_changed();
  void on_node_moved();

private:
  // True if connecting source -> target would create a cycle -- i.e.
  // target can already reach source by following zero or more existing
  // outgoing connections (a plain graph reachability search from target;
  // with merge points allowed, a node can have more than one parent, so
  // this can no longer just walk a single-parent chain) -- see
  // mouseReleaseEvent().
  bool would_create_cycle(QuestionNode *source, QuestionNode *target) const;

  // Removes connection from connections_ and un-marks both endpoints'
  // sides, but does NOT delete it -- the caller deletes the item once it's
  // safely out of that container. Used by remove_connection(); remove_node()
  // has its own inline version since it erases several connections in one
  // pass while iterating the same container.
  void detach_connection(ConnectionItem *connection);

  std::vector<QuestionNode *> nodes_;
  std::vector<ConnectionItem *> connections_;

  // Non-null exactly while a connection is being dragged from a side --
  // see mousePress/Move/ReleaseEvent(). pending_source_side_ is which of
  // pending_source_'s sides it started from (fixed for the whole drag --
  // the line snaps from there, it doesn't follow the cursor's side of the
  // box). pending_line_ is the temporary dashed rubber-band item following
  // the cursor during that drag.
  QuestionNode *pending_source_ = nullptr;
  QuestionNode::Side pending_source_side_ = QuestionNode::Side::Top;
  QGraphicsPathItem *pending_line_ = nullptr;
};
