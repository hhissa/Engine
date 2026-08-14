#pragma once
#include <QGraphicsObject>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>
#include <vector>

// One node in the conversation graph -- a question and its answer lines,
// drawn as a single box that reads top-to-bottom as the actual dialogue
// flow: the question text, then every answer line under it in order (numbered
// 1., 2., ...) -- "Question -> answer part 1 -> answer part 2 -> ...". The
// box grows to fit however much text that is (see content_height()); it's
// never truncated to a fixed-size summary.
//
// Whether it's a top-level question or a follow-up (and of what) is never
// stored here: that's purely a function of GraphScene's connections (see
// its own class comment) -- a node with no incoming connection is
// top-level, one with an incoming connection from node P is P's
// follow-up, and that connection is what continues the flow on to the
// *next* question once this one's answer finishes. Nothing stops a node
// from also having its own outgoing connection to yet another node, so a
// straight chain (Q1 -> Q2 -> Q3 -> Q4, each with exactly one parent and
// one child) is just as valid a shape as a branching tree -- and neither
// direction is capped at all, so several different questions can all
// connect into the very same node too (a merge point: whichever of those
// branches the player actually took, they end up at the same shared next
// question once it's exhausted).
//
// A red-bordered rounded-rect box, draggable (QGraphicsItem::ItemIsMovable)
// and selectable; emits moved() on every position change (see
// itemChange()) so GraphScene can keep connected ConnectionItems' curves
// attached while it's dragged.
//
// Connections attach at the midpoint of one of the box's four sides (see
// Side below), never an arbitrary point along the edge -- both to keep
// multiple connections on one node visually distinct and so GraphScene can
// snap a drop to whichever side is closest instead of demanding
// pixel-perfect accuracy. A side's small handle circle (see paint()) is
// normally hidden; hovering the node reveals all four (as an affordance
// for "you can drag from any of these"), and a side stays revealed even
// without hovering once GraphScene has actually attached a connection
// there (see mark_side_connected()) -- so an established connection point
// remains visible, not just a fleeting hover hint.
class QuestionNode : public QGraphicsObject {
  Q_OBJECT
public:
  // qgraphicsitem_cast<QuestionNode*>() (used throughout GraphScene to
  // tell a node apart from a ConnectionItem/the temporary rubber-band line
  // under the cursor) only actually checks the item's runtime type when a
  // class overrides type()/Type like this -- left at the QGraphicsItem
  // base default, qgraphicsitem_cast<QuestionNode*>() degenerates into an
  // unconditional cast that "succeeds" on *any* item, including a
  // QGraphicsPathItem, silently reinterpreting it as a QuestionNode* and
  // corrupting memory the moment a QuestionNode-only member is touched on
  // it. This is what crashed on dropping a connection.
  enum { Type = UserType + 1 };
  int type() const override { return Type; }

  enum class Side { Top, Bottom, Left, Right };
  static constexpr int kSideCount = 4;
  static constexpr Side kSides[kSideCount] = {Side::Top, Side::Bottom, Side::Left,
                                              Side::Right};

  explicit QuestionNode(QGraphicsItem *parent = nullptr);

  QRectF boundingRect() const override;
  void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
            QWidget *widget) override;

  QString question() const { return question_; }
  void set_question(QString text);

  QStringList answer_lines() const { return answer_lines_; }
  void set_answer_lines(QStringList lines);

  // Opaque symbolic name (see resources/conversation.h's `tag=` line/
  // ConversationQuestion::tag) a game's dialogue system can react to --
  // empty means "no tag" (mirrors ConversationQuestion::tag's
  // std::nullopt, see graph_scene.cpp's place()/to_question()).
  QString tag() const { return tag_; }
  void set_tag(QString tag);

  // Flag names this question requires set/unset before it's selectable at
  // all in the game (see resources/conversation.h's `requires=`/
  // `requires_not=` lines/ConversationQuestion::requires_flags/
  // requires_not_flags) -- empty means unconditionally selectable, the
  // common case. Same "stored for conversation_io.cpp, not painted on the
  // node face" treatment as tag() above.
  QStringList requires_flags() const { return requires_flags_; }
  void set_requires_flags(QStringList flags);
  QStringList requires_not_flags() const { return requires_not_flags_; }
  void set_requires_not_flags(QStringList flags);

  // Flag names set the instant this question is asked in-game (see
  // `sets=`/ConversationQuestion::sets_flags).
  QStringList sets_flags() const { return sets_flags_; }
  void set_sets_flags(QStringList flags);

  // Whether this question is a story ending (see the bare `ending` line/
  // ConversationQuestion::is_ending).
  bool is_ending() const { return is_ending_; }
  void set_is_ending(bool is_ending);

  // This ending's own outro text (see resources/conversation.h's
  // `ending_text=` lines/ConversationQuestion::ending_lines) -- only
  // meaningful when is_ending() is true, same as ending_text= itself is
  // only meaningful alongside a bare `ending` line, but stored/settable
  // regardless (nothing here enforces that pairing -- see conversation.h's
  // own comment). Same "stored for conversation_io.cpp, not painted on the
  // node face" treatment as answer_lines() above.
  QStringList ending_lines() const { return ending_lines_; }
  void set_ending_lines(QStringList lines);

  // Scene-space position of the midpoint of one of this box's four sides
  // (accounting for its current, content-dependent height) -- where a
  // ConnectionItem attached there actually starts/ends.
  QPointF side_scene_pos(Side side) const;

  // Current total content height (see layout_content()'s comment), at
  // least kMinHeight -- the actual on-screen height of this box right now,
  // as opposed to boundingRect()'s height which also includes the side
  // handle padding. Public so layout code (e.g. graph_scene.cpp's place())
  // can space rows of nodes apart by however tall each one actually grew
  // to fit its content, instead of guessing with a fixed constant.
  qreal content_height() const { return layout_content(nullptr, nullptr); }

  // The side (if any) within kSideGrabRadius of local_pos -- GraphScene
  // checks this (via a mapFromScene()'d press position) to decide whether
  // a press starts a new connection from that specific side, or (this
  // returning nullopt) drags the node instead, the ordinary
  // QGraphicsItem-movable behaviour.
  std::optional<Side> side_at(QPointF local_pos) const;

  // The side of this box whose midpoint is closest to scene_point -- used
  // to snap a connection's far endpoint to a specific side when the drop
  // position isn't necessarily right on top of one (see GraphScene::
  // mouseReleaseEvent()).
  Side nearest_side_to(QPointF scene_point) const;

  // Shows/hides all four side handles regardless of connection state --
  // GraphScene's hover events call this (see hoverEnterEvent()/
  // hoverLeaveEvent() below).
  void set_hovered(bool hovered);

  // Increments/decrements how many connections currently attach at side --
  // a side's handle stays visible whenever this is above zero, even
  // without hovering (see paint()). A count rather than a bool since more
  // than one connection can share a side (e.g. two follow-ups both leaving
  // from the same one).
  void mark_side_connected(Side side);
  void unmark_side_connected(Side side);

  static constexpr qreal kWidth = 220.0;
  // Floor for content_height() below -- a question with no answer lines
  // yet still gets a readable box, not a sliver.
  static constexpr qreal kMinHeight = 70.0;
  // Hit-test radius for side_at() -- deliberately larger than the drawn
  // handle circle (kSideHandleRadius) so grabbing one doesn't demand
  // pixel-perfect accuracy.
  static constexpr qreal kSideGrabRadius = 14.0;
  static constexpr qreal kSideHandleRadius = 6.0;

signals:
  // Emitted from itemChange() on ItemPositionHasChanged -- GraphScene
  // relays this to every ConnectionItem touching this node so their curves
  // follow it while it's dragged.
  void moved();

protected:
  QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
  void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override;
  void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;

private:
  // Lays out the question text and every numbered answer line within a
  // kWidth-wide, word-wrapped column, filling in exactly where paint()
  // should draw each one (question_rect_out/answer_rects_out, each
  // optional -- pass nullptr if only the total height is wanted) and
  // returning the box's total content height (at least kMinHeight). The
  // single source of truth both boundingRect()/content_height() (need
  // just the height) and paint() (needs the exact rects too) go through,
  // so the two can never disagree about how tall the box is.
  qreal layout_content(QRectF *question_rect_out,
                       std::vector<QRectF> *answer_rects_out) const;

  // Node-local (unmapped) position of side's midpoint, given the box's
  // *current* content_height() -- the shared basis side_scene_pos()/
  // side_at()/nearest_side_to() all build on.
  QPointF side_local_pos(Side side) const;

  QString question_ = QStringLiteral("New question");
  QStringList answer_lines_;
  QString tag_;
  QStringList requires_flags_;
  QStringList requires_not_flags_;
  QStringList sets_flags_;
  bool is_ending_ = false;
  QStringList ending_lines_;

  bool hovered_ = false;
  std::array<int, kSideCount> side_connection_count_{};
};
