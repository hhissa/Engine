#include "main_window.h"
#include "conversation_io.h"
#include "graph_scene.h"
#include "question_node.h"

#include <resources/conversation.h>

#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

GraphView::GraphView(GraphScene *scene, QWidget *parent) : QGraphicsView(scene, parent) {
  setRenderHint(QPainter::Antialiasing);
  setDragMode(QGraphicsView::RubberBandDrag); // click-drag empty space -> box-select
}

void GraphView::wheelEvent(QWheelEvent *event) {
  constexpr qreal kZoomStep = 1.15;
  qreal factor = event->angleDelta().y() > 0 ? kZoomStep : 1.0 / kZoomStep;
  scale(factor, factor);
}

ConversationEditorWindow::ConversationEditorWindow() {
  setWindowTitle("Conversation Editor");
  // Explicit and unbounded -- guards against this window ever ending up
  // effectively fixed-size (maximumSize() defaults to QWIDGETSIZE_MAX
  // already, but leaving it implicit means a stray setFixedSize()/
  // setMaximumSize() added anywhere later, or a layout size-constraint
  // change, could silently reintroduce a "can't resize/maximize" bug).
  setMinimumSize(0, 0);
  setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
  resize(1100, 700);

  auto *central = new QWidget(this);
  central->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  auto *root_layout = new QHBoxLayout(central);

  auto *middle_panel = new QVBoxLayout();
  auto *button_row = new QHBoxLayout();
  auto *new_button = new QPushButton("New");
  auto *add_button = new QPushButton("Add Question");
  auto *delete_button = new QPushButton("Delete Selected");
  auto *load_button = new QPushButton("Load...");
  auto *save_button = new QPushButton("Save...");
  connect(new_button, &QPushButton::clicked, this, &ConversationEditorWindow::on_new_clicked);
  connect(add_button, &QPushButton::clicked, this,
         &ConversationEditorWindow::on_add_question_clicked);
  connect(delete_button, &QPushButton::clicked, this,
         &ConversationEditorWindow::on_delete_selected_clicked);
  connect(load_button, &QPushButton::clicked, this, &ConversationEditorWindow::on_load_clicked);
  connect(save_button, &QPushButton::clicked, this, &ConversationEditorWindow::on_save_clicked);
  button_row->addWidget(new_button);
  button_row->addWidget(add_button);
  button_row->addWidget(delete_button);
  button_row->addStretch(1);
  button_row->addWidget(load_button);
  button_row->addWidget(save_button);
  middle_panel->addLayout(button_row);

  scene_ = new GraphScene(this);
  connect(scene_, &GraphScene::node_selected, this,
         &ConversationEditorWindow::on_node_selected);
  view_ = new GraphView(scene_, central);
  // Just a sane floor (scrollbars handle anything smaller than the graph
  // itself needs) -- kept modest so it can't itself become the thing
  // pinning the window's minimum width too wide for a smaller screen.
  view_->setMinimumSize(240, 180);
  view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  middle_panel->addWidget(view_, /*stretch=*/1);
  root_layout->addLayout(middle_panel, /*stretch=*/3);

  auto *side_panel = new QVBoxLayout();
  auto *form_group = new QGroupBox("Selected Question");
  auto *form = new QFormLayout(form_group);

  question_edit_ = new QLineEdit();
  connect(question_edit_, &QLineEdit::textChanged, this,
         &ConversationEditorWindow::on_question_text_changed);
  form->addRow("Question:", question_edit_);

  answers_list_ = new QListWidget();
  answers_list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  connect(answers_list_, &QListWidget::itemChanged, this,
         &ConversationEditorWindow::on_answer_lines_changed);
  form->addRow("Answer Lines:", answers_list_);

  auto *answer_button_row = new QHBoxLayout();
  auto *add_answer_button = new QPushButton("Add Line");
  auto *remove_answer_button = new QPushButton("Remove Line");
  connect(add_answer_button, &QPushButton::clicked, this,
         &ConversationEditorWindow::on_add_answer_line_clicked);
  connect(remove_answer_button, &QPushButton::clicked, this,
         &ConversationEditorWindow::on_remove_answer_line_clicked);
  answer_button_row->addWidget(add_answer_button);
  answer_button_row->addWidget(remove_answer_button);
  form->addRow("", answer_button_row);

  side_panel->addWidget(form_group);
  auto *hint_label = new QLabel(
      "Hover a node to reveal its 4 side handles, then drag one to "
      "another node to make it a follow-up (it snaps to the nearest "
      "side). Chain questions Q1 -> Q2 -> Q3 the same way. A node with "
      "no incoming connection is top-level.");
  // Without this, a QLabel's minimum width is its full text laid out on
  // one line -- for a sentence this long, that's easily wider than a
  // laptop screen, which is exactly what was pinning this whole window's
  // minimum width too wide to shrink, resize horizontally, or maximize
  // into. Word-wrapping lets it (and therefore the window) shrink down to
  // roughly its longest unbreakable word instead.
  hint_label->setWordWrap(true);
  side_panel->addWidget(hint_label);
  side_panel->addStretch(1);
  root_layout->addLayout(side_panel, /*stretch=*/1);

  setCentralWidget(central);

  set_fields_enabled(false);
}

ConversationEditorWindow::~ConversationEditorWindow() {
  // scene_ is a QObject child of `this` (see its constructor:
  // `new GraphScene(this)`), so Qt deletes it automatically as part of
  // this object's own teardown -- but only during the *base-class* portion
  // of that sequence, which runs after ConversationEditorWindow's own
  // destructor body has already finished. Deleting a QuestionNode that's
  // currently selected (see GraphScene::~GraphScene()/clear_graph(), which
  // scene_'s own destructor runs) fires selectionChanged() ->
  // GraphScene::on_selection_changed() -> node_selected(), which was
  // connected straight to this->on_node_selected() -- reaching a receiver
  // whose derived-class identity has already partially unwound, which Qt
  // detects and asserts on rather than risk invoking. Disconnecting here,
  // while this object is still fully itself, avoids that (the exact same
  // hazard, and the same fix, as GraphScene's own destructor comment
  // describes for its QuestionNode children).
  if (scene_) {
    scene_->disconnect(this);
  }
}

void ConversationEditorWindow::on_add_question_clicked() {
  QPointF pos(30.0 + static_cast<qreal>(add_count_ % 8) * 40.0,
             30.0 + static_cast<qreal>(add_count_ % 8) * 40.0);
  ++add_count_;
  QuestionNode *node = scene_->add_node(pos);
  scene_->clearSelection();
  node->setSelected(true);
}

void ConversationEditorWindow::on_delete_selected_clicked() {
  if (!selected_node_) {
    return;
  }
  scene_->remove_node(selected_node_);
  selected_node_ = nullptr;
  set_fields_enabled(false);
}

void ConversationEditorWindow::on_new_clicked() {
  scene_->clear_graph();
  selected_node_ = nullptr;
  add_count_ = 0;
  set_fields_enabled(false);
}

void ConversationEditorWindow::on_load_clicked() {
  QString path = QFileDialog::getOpenFileName(
      this, "Load Conversation", "assets/conversations/",
      "Conversation Files (*.conversation)");
  if (path.isEmpty()) {
    return;
  }
  std::optional<Conversation> loaded = load_conversation_file(path.toStdString());
  if (!loaded) {
    QMessageBox::warning(this, "Load Failed", "Could not read " + path);
    return;
  }
  scene_->load_from_conversation(*loaded);
  selected_node_ = nullptr;
  add_count_ = 0;
  set_fields_enabled(false);
}

void ConversationEditorWindow::on_save_clicked() {
  QString path = QFileDialog::getSaveFileName(
      this, "Save Conversation", "assets/conversations/dialogue.conversation",
      "Conversation Files (*.conversation)");
  if (path.isEmpty()) {
    return;
  }
  Conversation conversation = scene_->to_conversation();
  if (!save_conversation(path.toStdString(), conversation)) {
    QMessageBox::warning(this, "Save Failed", "Could not write to " + path);
  }
}

void ConversationEditorWindow::on_node_selected(QuestionNode *node) {
  selected_node_ = node;
  populate_fields_from_selection(node);
}

void ConversationEditorWindow::populate_fields_from_selection(QuestionNode *node) {
  populating_fields_ = true;

  if (node) {
    question_edit_->setText(node->question());
    answers_list_->clear();
    for (const QString &line : node->answer_lines()) {
      auto *item = new QListWidgetItem(line, answers_list_);
      item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    set_fields_enabled(true);
  } else {
    question_edit_->clear();
    answers_list_->clear();
    set_fields_enabled(false);
  }

  populating_fields_ = false;
}

void ConversationEditorWindow::set_fields_enabled(bool enabled) {
  question_edit_->setEnabled(enabled);
  answers_list_->setEnabled(enabled);
}

void ConversationEditorWindow::on_question_text_changed() {
  if (populating_fields_ || !selected_node_) {
    return;
  }
  selected_node_->set_question(question_edit_->text());
}

void ConversationEditorWindow::on_answer_lines_changed() {
  if (populating_fields_ || !selected_node_) {
    return;
  }
  QStringList lines;
  for (int i = 0; i < answers_list_->count(); ++i) {
    lines << answers_list_->item(i)->text();
  }
  selected_node_->set_answer_lines(lines);
}

void ConversationEditorWindow::on_add_answer_line_clicked() {
  if (!selected_node_) {
    return;
  }
  bool ok = false;
  QString text = QInputDialog::getText(this, "Add Answer Line", "Line text:",
                                       QLineEdit::Normal, QString(), &ok);
  if (!ok) {
    return;
  }
  auto *item = new QListWidgetItem(text, answers_list_);
  item->setFlags(item->flags() | Qt::ItemIsEditable);
  on_answer_lines_changed(); // itemChanged doesn't fire for a freshly
                            // inserted item, only edits to an existing one
}

void ConversationEditorWindow::on_remove_answer_line_clicked() {
  QListWidgetItem *item = answers_list_->currentItem();
  if (!item) {
    return;
  }
  delete answers_list_->takeItem(answers_list_->row(item));
  on_answer_lines_changed();
}
