#include "question_node.h"

#include <QColor>
#include <QFontMetrics>
#include <QGraphicsSceneHoverEvent>
#include <QPainter>
#include <QPen>
#include <QStyle>
#include <QStyleOptionGraphicsItem>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr qreal kPadding = 12.0;
constexpr qreal kDividerGap = 8.0;  // above and below the divider line
constexpr qreal kAnswerLineGap = 4.0; // between consecutive answer lines

QFont question_font_for(const QFont &base) {
  QFont font = base;
  font.setBold(true);
  return font;
}

QFont answer_font_for(const QFont &base) {
  QFont font = base;
  font.setBold(false);
  font.setPointSizeF(font.pointSizeF() * 0.9);
  return font;
}
} // namespace

constexpr QuestionNode::Side QuestionNode::kSides[QuestionNode::kSideCount];

QuestionNode::QuestionNode(QGraphicsItem *parent) : QGraphicsObject(parent) {
  setFlag(ItemIsMovable);
  setFlag(ItemIsSelectable);
  setFlag(ItemSendsGeometryChanges); // needed for itemChange() position notifications
  setAcceptHoverEvents(true); // for set_hovered()'s side-handle reveal
  setZValue(1.0); // draw over connection splines
}

qreal QuestionNode::layout_content(QRectF *question_rect_out,
                                   std::vector<QRectF> *answer_rects_out) const {
  qreal content_width = kWidth - kPadding * 2.0;
  QRectF measure_bounds(0.0, 0.0, content_width, std::numeric_limits<qreal>::max() / 2.0);

  QFontMetricsF question_metrics{question_font_for(QFont())};
  QRectF question_rect =
      question_metrics.boundingRect(measure_bounds, Qt::TextWordWrap, question_);
  question_rect.moveTopLeft(QPointF(kPadding, kPadding));
  if (question_rect_out) {
    *question_rect_out = question_rect;
  }

  qreal y = question_rect.bottom();

  if (!answer_lines_.isEmpty()) {
    y += kDividerGap * 2.0; // clearance above and below the divider line itself
    QFontMetricsF answer_metrics{answer_font_for(QFont())};
    for (int i = 0; i < answer_lines_.size(); ++i) {
      QString numbered = QStringLiteral("%1. %2").arg(i + 1).arg(answer_lines_[i]);
      QRectF rect = answer_metrics.boundingRect(measure_bounds, Qt::TextWordWrap, numbered);
      rect.moveTopLeft(QPointF(kPadding, y));
      if (answer_rects_out) {
        answer_rects_out->push_back(rect);
      }
      y = rect.bottom() + kAnswerLineGap;
    }
    y -= kAnswerLineGap; // drop the last line's trailing gap
  }

  return std::max(y + kPadding, kMinHeight);
}

QRectF QuestionNode::boundingRect() const {
  // Padded enough beyond the visible box for a side handle circle
  // (centered exactly on its side's midpoint, so it extends
  // kSideHandleRadius outside the box on that one side) plus the pen
  // width/selection outline, so neither is clipped.
  qreal pad = kSideHandleRadius + 3.0;
  return QRectF(-pad, -pad, kWidth + pad * 2.0, content_height() + pad * 2.0);
}

void QuestionNode::paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
                         QWidget * /*widget*/) {
  painter->setRenderHint(QPainter::Antialiasing);

  QRectF question_rect;
  std::vector<QRectF> answer_rects;
  qreal height = layout_content(&question_rect, &answer_rects);

  QRectF body(0.0, 0.0, kWidth, height);
  bool selected = option->state & QStyle::State_Selected;

  painter->setPen(QPen(selected ? QColor(255, 200, 90) : QColor(210, 40, 40),
                       selected ? 3.0 : 2.0));
  painter->setBrush(QColor(45, 45, 52));
  painter->drawRoundedRect(body, 10.0, 10.0);

  painter->setPen(QColor(240, 240, 240));
  painter->setFont(question_font_for(painter->font()));
  painter->drawText(question_rect, Qt::TextWordWrap, question_);

  if (!answer_lines_.isEmpty()) {
    qreal divider_y = question_rect.bottom() + kDividerGap;
    painter->setPen(QPen(QColor(110, 55, 55), 1.0));
    painter->drawLine(QPointF(kPadding, divider_y), QPointF(kWidth - kPadding, divider_y));

    painter->setFont(answer_font_for(painter->font()));
    painter->setPen(QColor(205, 205, 210));
    for (int i = 0; i < answer_lines_.size(); ++i) {
      QString numbered = QStringLiteral("%1. %2").arg(i + 1).arg(answer_lines_[i]);
      painter->drawText(answer_rects[static_cast<size_t>(i)], Qt::TextWordWrap, numbered);
    }
  }

  // Side connection handles -- hidden unless hovered_ or this side already
  // has a live connection (see mark_side_connected()), and tinted
  // differently between the two so an established connection point reads
  // distinctly from a fleeting hover hint.
  for (int i = 0; i < kSideCount; ++i) {
    bool connected = side_connection_count_[static_cast<size_t>(i)] > 0;
    if (!hovered_ && !connected) {
      continue;
    }
    painter->setPen(QPen(QColor(60, 60, 68), 1.5));
    painter->setBrush(connected ? QColor(120, 220, 140) : QColor(120, 190, 255));
    painter->drawEllipse(side_local_pos(kSides[i]), kSideHandleRadius, kSideHandleRadius);
  }
}

void QuestionNode::set_question(QString text) {
  prepareGeometryChange(); // box height may change -- see boundingRect()
  question_ = std::move(text);
  update();
}

void QuestionNode::set_answer_lines(QStringList lines) {
  prepareGeometryChange(); // box height may change -- see boundingRect()
  answer_lines_ = std::move(lines);
  update();
}

QPointF QuestionNode::side_local_pos(Side side) const {
  qreal h = content_height();
  switch (side) {
  case Side::Top:
    return QPointF(kWidth * 0.5, 0.0);
  case Side::Bottom:
    return QPointF(kWidth * 0.5, h);
  case Side::Left:
    return QPointF(0.0, h * 0.5);
  case Side::Right:
    return QPointF(kWidth, h * 0.5);
  }
  return QPointF(0.0, 0.0); // unreachable -- silences a "not all paths return" warning
}

QPointF QuestionNode::side_scene_pos(Side side) const {
  return mapToScene(side_local_pos(side));
}

std::optional<QuestionNode::Side> QuestionNode::side_at(QPointF local_pos) const {
  for (Side side : kSides) {
    QPointF delta = local_pos - side_local_pos(side);
    if (QPointF::dotProduct(delta, delta) <= kSideGrabRadius * kSideGrabRadius) {
      return side;
    }
  }
  return std::nullopt;
}

QuestionNode::Side QuestionNode::nearest_side_to(QPointF scene_point) const {
  QPointF local = mapFromScene(scene_point);
  Side best = Side::Top;
  qreal best_dist = std::numeric_limits<qreal>::max();
  for (Side side : kSides) {
    QPointF delta = local - side_local_pos(side);
    qreal dist = QPointF::dotProduct(delta, delta);
    if (dist < best_dist) {
      best_dist = dist;
      best = side;
    }
  }
  return best;
}

void QuestionNode::set_hovered(bool hovered) {
  if (hovered_ != hovered) {
    hovered_ = hovered;
    update();
  }
}

void QuestionNode::mark_side_connected(Side side) {
  ++side_connection_count_[static_cast<size_t>(side)];
  update();
}

void QuestionNode::unmark_side_connected(Side side) {
  int &count = side_connection_count_[static_cast<size_t>(side)];
  if (count > 0) {
    --count;
  }
  update();
}

void QuestionNode::hoverEnterEvent(QGraphicsSceneHoverEvent *event) {
  set_hovered(true);
  QGraphicsObject::hoverEnterEvent(event);
}

void QuestionNode::hoverLeaveEvent(QGraphicsSceneHoverEvent *event) {
  set_hovered(false);
  QGraphicsObject::hoverLeaveEvent(event);
}

QVariant QuestionNode::itemChange(GraphicsItemChange change, const QVariant &value) {
  if (change == ItemPositionHasChanged) {
    emit moved();
  }
  return QGraphicsObject::itemChange(change, value);
}
