#pragma once
#include "question_node.h"

#include <QGraphicsPathItem>

// A directed edge in the conversation graph: target is a follow-up of
// source -- see GraphScene's own class comment for exactly what that
// means (single incoming connection per node; a node with none is
// top-level; nothing stops chaining Q1 -> Q2 -> Q3 -> Q4, each node's
// single outgoing connection feeding the next). Drawn as a cubic-Bezier
// spline (QPainterPath::cubicTo(), the usual node-editor look) between the
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

  ConnectionItem(QuestionNode *source, QuestionNode::Side source_side,
                QuestionNode *target, QuestionNode::Side target_side);

  QuestionNode *source() const { return source_; }
  QuestionNode *target() const { return target_; }
  QuestionNode::Side source_side() const { return source_side_; }
  QuestionNode::Side target_side() const { return target_side_; }

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
};
