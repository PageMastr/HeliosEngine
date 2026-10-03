#pragma once
// Internal mutable access to documents and the workspace (only transactions and the Framework
// use it; see document.h).

#include <memory>
#include <utility>

#include "helios/toolsfw/document.h"

namespace helios::tf {

struct DocAccess {
    static void* object(Document& d) noexcept { return d.m_value.data(); }
    static refl::RecordHeader& header(Document& d) noexcept { return d.m_header; }
    /// Marks the document changed (new revision; caches invalidated).
    static void touch(Document& d) noexcept { d.touch(); }
    static void setSavedHash(Document& d, u64 hash) noexcept { d.m_savedHash = hash; }
    /// Records `text` (canonical) as the on-disk base: savedHash = hash(text).
    static void setBase(Document& d, std::string text);
    static void clearBase(Document& d) noexcept {
        d.m_baseText.clear();
        d.m_savedHash = 0;
    }
    static void setDestroyed(Document& d, bool destroyed) noexcept {
        d.m_destroyed = destroyed;
        d.touch();
    }
    static void setPath(Document& d, fs::Path absolute, std::string relative) {
        d.m_path = std::move(absolute);
        d.m_relativePath = std::move(relative);
    }

    static Document* add(Workspace& ws, std::unique_ptr<Document> doc) {
        Document* raw = doc.get();
        ws.m_docs.push_back(std::move(doc));
        ws.m_order.push_back(raw);
        return raw;
    }
    static std::unique_ptr<Document> remove(Workspace& ws, const DocId& id) {
        for (usize i = 0; i < ws.m_docs.size(); ++i) {
            if (ws.m_docs[i]->id() == id) {
                std::unique_ptr<Document> doc = std::move(ws.m_docs[i]);
                ws.m_docs.erase(ws.m_docs.begin() + static_cast<isize>(i));
                std::erase(ws.m_order, doc.get());
                return doc;
            }
        }
        return nullptr;
    }
    static Document* find(Workspace& ws, const DocId& id) noexcept { return ws.find(id); }
};

} // namespace helios::tf
