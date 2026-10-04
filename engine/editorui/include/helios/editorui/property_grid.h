#pragma once
// Property grid (07 §1.4): the generic inspector over reflected documents. Rows come from TypeInfo
// and the schema's editor metadata: names, doc-comment tooltips, `@unit` suffixes, `@range` clamps,
// enums as combos, flags as check boxes, structs, lists, keyed lists (with add, remove and reorder),
// maps, optionals and variants, and `client`/`server` badges. Every edit is a command
// (doc.setProperty, doc.insertElement, doc.remove, doc.moveElement) through the caller's invoker,
// so it is a transaction with that input path's origin; continuous drags share a mergeKey and undo
// as one step.
//
// Item paths for helios-uitest: "<window>/Grid/row[<property path>]/value" for a row's editor,
// ".../name" for its label cell, ".../add" and ".../remove" for list buttons.
//
// Threading: UI thread, inside an ImGui frame.

#include <string>

#include "helios/core/types.h"

namespace helios::tf {
class Framework;
class CommandInvoker;
class Document;
} // namespace helios::tf

namespace helios::edui {

class PropertyGrid {
public:
    /// Draws the grid of `doc` into the current window.
    void draw(tf::Framework& framework, tf::CommandInvoker& invoker, const tf::Document& doc);
    /// The last failed edit's message ("" after a successful one).
    const std::string& lastError() const noexcept { return m_error; }

    struct Impl;  ///< Internal.

private:
    u64 m_gesture = 0;
    u32 m_activeId = 0;
    std::string m_error;
    std::string m_newKey;
};

} // namespace helios::edui
