#include "connection_item.h"

#include <QColor>
#include <QPainterPath>
#include <QPen>
#include <QPointF>

#include <algorithm>
#include <cmath>

namespace {
// A fixed outward direction per side (pointing straight away from the
// box, perpendicular to that side) -- used as the cubic-Bezier
// control-point pull direction in update_path() below, so the curve
// always leaves/enters a side looking like it's actually emerging from
// it, regardless of where the other end of the connection currently is.
QPointF outward_direction(QuestionNode::Side side) {
  switch (side) {
  case QuestionNode::Side::Top:
    return QPointF(0.0, -1.0);
  case QuestionNode::Side::Bottom:
    return QPointF(0.0, 1.0);
  case QuestionNode::Side::Left:
    return QPointF(-1.0, 0.0);
  case QuestionNode::Side::Right:
    return QPointF(1.0, 0.0);
  }
  return QPointF(0.0, 1.0); // unreachable
}
} // namespace

ConnectionItem::ConnectionItem(QuestionNode *source, QuestionNode::Side source_side,
                              QuestionNode *target, QuestionNode::Side target_side)
    : source_(source), target_(target), source_side_(source_side),
      target_side_(target_side) {
  setPen(QPen(QColor(150, 150, 160), 2.0));
  setZValue(0.0); // draw under nodes
  update_path();
}

void ConnectionItem::update_path() {
  QPointF p0 = source_->side_scene_pos(source_side_);
  QPointF p1 = target_->side_scene_pos(target_side_);

  // Control points pulled outward from each side's midpoint along its own
  // fixed perpendicular direction -- the standard node-editor S-curve.
  // Distance grows with how far apart the endpoints are so the curve
  // doesn't look overly sharp for widely separated nodes, clamped to a
  // sane range so neither adjacent-node nor across-the-canvas cases look
  // degenerate.
  qreal span = std::hypot(p1.x() - p0.x(), p1.y() - p0.y());
  qreal pull = std::clamp(span * 0.5, 40.0, 160.0);

  QPointF c1 = p0 + outward_direction(source_side_) * pull;
  QPointF c2 = p1 + outward_direction(target_side_) * pull;

  QPainterPath path(p0);
  path.cubicTo(c1, c2, p1);
  setPath(path);
}
