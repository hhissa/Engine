#pragma once
#include "question_node.h"

#include <QGraphicsPathItem>

// A directed edge in the conversation graph: normally (Kind::FollowUp)
// target is a follow-up of source -- see GraphScene's own class comment
// for exactly what that means (a node with no incoming FollowUp connection
// is top-level; nothing stops chaining Q1 -> Q2 -> Q3 -> Q4, each node's
// single outgoing connection feeding the next). A Kind::Loop connection
// means something different: source, once its answer finishes, jumps the
// player straight back to target instead of continuing to a follow-up or
// popping back up (see resources/conversation.h's `loop_to=`) -- target
// isn't "contained by" source the way a FollowUp's target is, it's just
// named as where to jump, so Loop connections are deliberately excluded
// from GraphScene's tree-shape bookkeeping (would_create_cycle(),
// has_incoming_connection(), to_question()'s follow_ups walk) -- see those
// functions' own comments. Drawn as a cubic-Bezier spline
// (QPainterPath::cubicTo(), the usual node-editor look) between the
// midpoint of one specific side of each box (see QuestionNode::Side --
// connections snap to whichever side's midpoint is nearest a drag/drop
// rather than an arbitrary point), kept in sync with either endpoint's
// position via update_path() -- GraphScene calls this whenever a connected
// node's moved() signal fires.
class ConnectionItem : public QGraphicsPathItem {
public:
  // See QuestionNode::Type's comment -- same reasoning, a distinct value
  // so qgraphicsitem_cast<ConnectionItem*>() (if ever used) and, more
  // importantly, qgraphicsitem_cast<QuestionNode*>() actually reject a
  // ConnectionItem instead of silently "succeeding" on it.
  enum { Type = UserType + 2 };
  int type() const override { return Type; }

  enum class Kind { FollowUp, Loop };

  ConnectionItem(QuestionNode *source, QuestionNode::Side source_side,
                QuestionNode *target, QuestionNode::Side target_side,
                Kind kind = Kind::FollowUp);

  QuestionNode *source() const { return source_; }
  QuestionNode *target() const { return target_; }
  QuestionNode::Side source_side() const { return source_side_; }
  QuestionNode::Side target_side() const { return target_side_; }
  Kind kind() const { return kind_; }

  // Recomputes the spline path from the two nodes' current positions --
  // call whenever either might have moved. The sides themselves never
  // change after construction (only which point in the *scene* each one
  // currently maps to does).
  void update_path();

private:
  QuestionNode *source_;
  QuestionNode *target_;
  QuestionNode::Side source_side_;
  QuestionNode::Side target_side_;
  Kind kind_;
};
