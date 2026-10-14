#pragma once
#include <defines.h>
#include <optional>
#include <string>
#include <string_view>

// One answer= line of a .conversation file, read as a line of *script*
// rather than as plain text -- see parse_script_line() below. The
// conversation format itself (resources/conversation.h) only knows an
// answer line as an opaque string; everything here is SH's own reading of
// it, the same way a `tag=` name means nothing to the parser and only
// something to the game.
//
// A line may open with any number of bracketed directives before its
// text, in any order:
//
//   answer=[Mel] Hi, I'm Mel, I'm 23...
//   answer=[title] What is your go-to meal?
//   answer=[Theo][cutaway cutaways/ramen] Oh let me put you on
//   answer=[Mel][photo photos/clouds/mel.jpg] You know those wispy ones?
//   answer=[overlay] Theres an hour
//   answer=[link https://...] The Moon by The Microphones
//   answer=[overlay][size 3][flash] Again
//   answer=[title][stack][size 2] Deeper
//   answer=[overlay][fill][flash] Again
//   answer=[overlay][at 0.8 0.2][size 1.6] and really high highs
//
//   [<Name>]        who is speaking. Sticky: a line with no name is said by
//                   whoever spoke last in the same answer. [P] is the
//                   photographer, who is never on screen -- see
//                   SHGame::on_script_line() for what a name does to the
//                   shot.
//   [title]         the text is shown alone on a black screen, the way an
//                   old film's intertitle is, instead of as dialogue.
//   [overlay]       the text is laid over the scene in the open space of
//                   the frame instead of in the bottom-left dialogue spot.
//   [cutaway <id>]  cut away from the speaker's room to the scene <id>
//                   (assets/scenes/<id>.sdf) for as long as they keep
//                   speaking.
//   [photo <path>]  same, but to a still image instead of a scene.
//   [link <url>]    the text names something the player can open (a song).
//   [size <scale>]  a [title] or [overlay] line drawn <scale> times the
//                   normal text size -- the script's "very large text", and
//                   its "text gets bigger every word" as a run of cards
//                   each a size up from the last.
//   [flash]         a [title] or [overlay] line that blinks.
//   [stack]         the script's "each word stacked one above the other":
//                   an [overlay] line stacks straight under the run's
//                   previous line rather than stepping across, and a run of
//                   [title][stack] cards keeps every earlier card's text up,
//                   each new one under the last.
//   [fill]          an [overlay] line shown alone, in capitals, centred and
//                   scaled up to fill the screen -- the script's "very
//                   large text". Ignores [size].
//   [center]        an [overlay] line centred across the screen; a run of
//                   them is centred down it as one block too.
//   [at <x> <y>]    an [overlay] line centred on that point of the screen,
//                   as fractions of its width and height (0 0 top-left,
//                   1 1 bottom-right) -- kept fully on screen.
//   [quiet]         a [title] card that lands without its ding.
//   [invert]        a [title] card drawn black on white instead of white
//                   on black -- the script's "bolded italic text is white
//                   background black text".
//   [room]          cut back from a cutaway/photo to the speaker's own room
//                   -- for when the speaker carries on past the moment the
//                   cutaway illustrated. See SHGame::on_script_line().
//
// A bracketed word that isn't one of the directives above is a speaker
// name, so a name never has to be registered anywhere before a file can
// use it.
struct ScriptLine {
  std::optional<std::string> speaker;
  bool title = false;
  bool overlay = false;
  std::optional<std::string> cutaway;
  std::optional<std::string> photo;
  std::optional<std::string> link;
  f32 size = 1.0f;
  bool flash = false;
  bool stack = false;
  bool room = false;
  bool fill = false;
  bool center = false;
  bool quiet = false;
  bool invert = false;
  bool has_at = false;
  f32 at_x = 0.5f; // see [at] -- only meaningful when has_at
  f32 at_y = 0.5f;
  std::string text; // what's left once every leading directive is removed
};

// The photographer's speaker name -- see ScriptLine's [P].
constexpr std::string_view kPhotographerSpeaker = "P";

// Splits raw's leading directives off its text -- see ScriptLine. A line
// with no directives at all comes back with only `text` set (and no
// speaker -- QASystem fills in the sticky one). A '[' that doesn't close
// stops directive parsing there and is kept as part of the text, so a
// stray bracket in dialogue is shown rather than swallowed.
ScriptLine parse_script_line(std::string_view raw);
