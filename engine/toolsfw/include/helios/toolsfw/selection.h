#pragma once
// Selection (07 §1.2): typed handles `ObjRef{doc, guid}` plus a sub-selection path (a property
// path, list element, graph node, key, ...), a back/forward history (Alt+Left/Right) and named
// sets. Selection changes are not document edits: they are not transactions and not journaled.
//
// Threading: owned by the Framework's owner thread.

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/toolsfw/types.h"

namespace helios::tf {

struct ObjRef {
    DocId doc;
    Guid guid;         ///< Object inside the document (entities, nodes); nil = the document itself.
    std::string path;  ///< Sub-selection: property path, "" = the whole object.
    friend bool operator==(const ObjRef&, const ObjRef&) = default;
};

class Selection {
public:
    /// Replaces the selection (a no-op when unchanged); records the previous one in the history.
    void set(std::vector<ObjRef> refs);
    void set(const ObjRef& ref) { set(std::vector<ObjRef>{ref}); }
    void add(const ObjRef& ref);
    void remove(const ObjRef& ref);
    void clear() { set(std::vector<ObjRef>{}); }

    std::span<const ObjRef> items() const noexcept { return m_current; }
    bool empty() const noexcept { return m_current.empty(); }
    bool contains(const ObjRef& ref) const noexcept;
    /// The first selected document (nil when nothing is selected).
    DocId primaryDocument() const noexcept;

    /// History navigation; false when there is nothing to go back/forward to.
    bool back();
    bool forward();
    bool canGoBack() const noexcept { return !m_back.empty(); }
    bool canGoForward() const noexcept { return !m_forward.empty(); }

    /// Named sets ("Hardpoints to check").
    void saveSet(std::string name);
    bool recallSet(std::string_view name);
    std::vector<std::string> setNames() const;

    /// Drops references into a closed or destroyed document (current, history and sets).
    void forgetDocument(const DocId& doc);
    /// Incremented on every change (UI refresh).
    u64 version() const noexcept { return m_version; }

    /// History depth kept per direction.
    static constexpr usize kHistoryLimit = 64;

private:
    void replace(std::vector<ObjRef> refs);
    std::vector<ObjRef> m_current;
    std::vector<std::vector<ObjRef>> m_back;
    std::vector<std::vector<ObjRef>> m_forward;
    std::map<std::string, std::vector<ObjRef>, std::less<>> m_sets;
    u64 m_version = 0;
};

} // namespace helios::tf
