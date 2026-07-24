#pragma once
#include <resources/conversation.h>

#include <string_view>

// Tool-side counterpart to engine/src/resources/conversation.h's
// load_conversation_file() -- writing has no engine-side equivalent (by
// design, mirroring how .sdf scene writing lives in testbed/src/
// sdf_authoring.cpp rather than engine/src/resources/sdf_scene.h itself):
// this tool is the only thing that ever authors .conversation files, so
// the writer lives here instead of in the engine.
//
// Writes conversation to path in exactly the format load_conversation_file()
// (engine/src/resources/conversation.h) parses back. Returns false if path
// couldn't be opened for writing.
//
// Question text isn't escaped on the way out -- the file format has no
// escape syntax (see conversation.h's comment: a question can't contain a
// literal '"'), so a question authored with one in the graph editor will
// round-trip incorrectly. Not worth solving here; the same "no escaping"
// simplicity already applies to every text-based file format in this
// engine (.sdf, .kmt).
bool save_conversation(std::string_view path, const Conversation &conversation);
