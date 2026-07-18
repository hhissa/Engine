#pragma once
#include <defines.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>

// The game's main text system: a question-and-answer dialogue.
//
// Starting state is a vertical block of questions. The player moves a
// cursor through them (Up/Down) and selects one (Enter); selecting a
// question permanently darkens it in the list and switches to showing that
// question's answer, one line at a time -- Enter cycles through the answer
// lines, and past the last line the system returns to the question list.
// Questions stay selectable after being asked (they're just darkened), so
// the player can re-read an answer.
//
// update() reads input and must be called once per frame from the game's
// update(); render() queues its text via renderer_draw_text() and must be
// called once per frame from the game's render() (queued draws don't
// persist across frames -- see renderer_frontend.h).
class QASystem {
public:
  // answer_lines is the text shown after selecting the question, one
  // element per Enter press.
  void add_entry(std::string question, std::vector<std::string> answer_lines);

  void update();
  void render() const;

private:
  enum class State {
    QuestionList, // the vertical block; cursor moves, Enter selects
    Answer,       // showing entries_[cursor_]'s answer; Enter advances
  };

  struct Entry {
    std::string question;
    std::vector<std::string> answer_lines;
    bool asked = false; // set on first selection -- darkens the question
                        // in the list permanently
  };

  std::vector<Entry> entries_;
  State state_ = State::QuestionList;
  size_t cursor_ = 0;      // which question the selection cursor is on
  size_t answer_line_ = 0; // current line within the open answer
};
