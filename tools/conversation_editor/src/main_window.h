#pragma once
#include <QGraphicsView>
#include <QMainWindow>
#include <QPoint>
#include <QPointF>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class GraphScene;
class QuestionNode;
class ConnectionItem;

// QGraphicsView with wheel-to-zoom and right-click-drag panning -- otherwise
// a plain view over GraphScene, left in RubberBandDrag mode throughout so
// left-click keeps doing its usual job (box-select over empty canvas, node
// body drag, and GraphScene's own side-handle connection drag -- all of
// which go through QGraphicsItem/QGraphicsScene machinery untouched by any
// of this). Kept here rather than its own file pair: it's a small set of
// overrides, not worth a second header/source for.
class GraphView : public QGraphicsView {
  Q_OBJECT
public:
  explicit GraphView(GraphScene *scene, QWidget *parent = nullptr);

protected:
  void wheelEvent(QWheelEvent *event) override;
  // Right button starts/stops a pan; panning_ + last_pan_pos_ track it
  // across the move events in between. Handled by hand (scrollbar deltas)
  // rather than via QGraphicsView::ScrollHandDrag, since that mode is
  // wired to the left button and switching it in and out per-press is what
  // broke left-click's own drag gestures (box-select, node-drag,
  // side-handle connection-drag) before.
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;

private:
  // Right-clicking directly on a ConnectionItem shows a context menu
  // ("Insert Node Here" / "Delete Connection") instead of starting the
  // usual right-click pan -- called from mousePressEvent() before it falls
  // through to the panning_ = true path below. click_scene_pos is where the
  // click landed in scene coordinates, both for GraphScene::
  // insert_node_on_connection()'s pos argument and to reuse as the menu's
  // own popup position (global_pos).
  void show_connection_menu(ConnectionItem *connection, QPointF click_scene_pos,
                            QPoint global_pos);

  GraphScene *scene_;
  bool panning_ = false;
  QPoint last_pan_pos_;
};

// A Qt node-graph editor for authoring .conversation files (see
// engine/src/resources/conversation.h for the format) -- build a question
// as a node, hover it to reveal the four side handles at its edge
// midpoints, and drag from one to another node to make that node a
// follow-up sub-question, and save.
// Nodes with no incoming connection end up as top-level questions in the
// exported file; nodes connected to them become subquestions, to any
// depth -- a straight Q1 -> Q2 -> Q3 -> Q4 chain works exactly the same
// way, one connection at a time (see GraphScene's class comment for
// exactly how the graph maps onto the file's nested structure).
//
// Run from bin/ (like sdf_editor) if you want relative asset paths in
// Load/Save dialogs to line up with where the game actually reads
// .conversation files from (assets/conversations/) -- unlike sdf_editor,
// this tool doesn't touch the engine's renderer/materials/textures at all,
// so it has no other working-directory requirement of its own.
class ConversationEditorWindow : public QMainWindow {
  Q_OBJECT

public:
  ConversationEditorWindow();
  // See the .cpp for why this can't just be left implicit -- scene_'s
  // node_selected() signal reaching back into this object during Qt's
  // automatic child-QObject cleanup (which runs after this class's own
  // destructor body has already finished) is a real, reproducible crash.
  ~ConversationEditorWindow() override;

private slots:
  void on_add_question_clicked();
  void on_delete_selected_clicked();
  void on_new_clicked();
  void on_load_clicked();
  void on_save_clicked();

  // Connected to scene_'s node_selected(QuestionNode*) signal -- populates
  // the side panel from node (or clears/disables it if node is nullptr,
  // i.e. selection dropped to zero or more-than-one nodes).
  void on_node_selected(QuestionNode *node);

  // Connected to question_edit_'s textChanged and answers_list_'s
  // itemChanged -- applies the side panel's current values onto
  // selected_node_ live, mirroring sdf_editor's on_live_edit_changed()
  // pattern. No-op if nothing is selected (populating_fields_ guards
  // against populate_fields_from_selection()'s own setText()/etc. calls
  // re-triggering this).
  void on_question_text_changed();
  void on_answer_lines_changed();
  void on_add_answer_line_clicked();
  void on_remove_answer_line_clicked();
  // Connected to tag_edit_'s textChanged -- mirrors
  // on_question_text_changed() exactly, for the optional `tag=` field (see
  // QuestionNode::tag()/resources/conversation.h's file format).
  void on_tag_changed();

private:
  void populate_fields_from_selection(QuestionNode *node);
  void set_fields_enabled(bool enabled);

  GraphScene *scene_ = nullptr;
  GraphView *view_ = nullptr;

  QuestionNode *selected_node_ = nullptr;

  QLineEdit *question_edit_ = nullptr;
  QListWidget *answers_list_ = nullptr;
  // Optional symbolic name a game's dialogue system can react to (see
  // QuestionNode::tag()) -- empty means no tag.
  QLineEdit *tag_edit_ = nullptr;

  // Guards populate_fields_from_selection()'s setText()/etc. calls against
  // re-entering on_question_text_changed()/on_answer_lines_changed() --
  // same idiom as sdf_editor's populating_fields_.
  bool populating_fields_ = false;

  // Staggers each successive "Add Question" node so they don't all land
  // in exactly the same spot -- reset by on_new_clicked()/on_load_clicked().
  int add_count_ = 0;
};
