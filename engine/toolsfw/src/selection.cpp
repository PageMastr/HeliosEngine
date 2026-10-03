#include "helios/toolsfw/selection.h"

#include <algorithm>

namespace helios::tf {

void Selection::replace(std::vector<ObjRef> refs) {
    m_current = std::move(refs);
    ++m_version;
}

void Selection::set(std::vector<ObjRef> refs) {
    if (refs == m_current) return;
    m_back.push_back(m_current);
    if (m_back.size() > kHistoryLimit) m_back.erase(m_back.begin());
    m_forward.clear();
    replace(std::move(refs));
}

void Selection::add(const ObjRef& ref) {
    if (contains(ref)) return;
    std::vector<ObjRef> next = m_current;
    next.push_back(ref);
    set(std::move(next));
}

void Selection::remove(const ObjRef& ref) {
    if (!contains(ref)) return;
    std::vector<ObjRef> next = m_current;
    std::erase(next, ref);
    set(std::move(next));
}

bool Selection::contains(const ObjRef& ref) const noexcept {
    return std::find(m_current.begin(), m_current.end(), ref) != m_current.end();
}

DocId Selection::primaryDocument() const noexcept {
    return m_current.empty() ? DocId{} : m_current.front().doc;
}

bool Selection::back() {
    if (m_back.empty()) return false;
    m_forward.push_back(m_current);
    std::vector<ObjRef> prev = std::move(m_back.back());
    m_back.pop_back();
    replace(std::move(prev));
    return true;
}

bool Selection::forward() {
    if (m_forward.empty()) return false;
    m_back.push_back(m_current);
    std::vector<ObjRef> next = std::move(m_forward.back());
    m_forward.pop_back();
    replace(std::move(next));
    return true;
}

void Selection::saveSet(std::string name) {
    m_sets[std::move(name)] = m_current;
    ++m_version;
}

bool Selection::recallSet(std::string_view name) {
    const auto it = m_sets.find(name);
    if (it == m_sets.end()) return false;
    set(it->second);
    return true;
}

std::vector<std::string> Selection::setNames() const {
    std::vector<std::string> out;
    for (const auto& [name, refs] : m_sets) out.push_back(name);
    return out;
}

void Selection::forgetDocument(const DocId& doc) {
    const auto strip = [&](std::vector<ObjRef>& refs) {
        return std::erase_if(refs, [&](const ObjRef& r) { return r.doc == doc; }) != 0;
    };
    bool changed = strip(m_current);
    for (auto& h : m_back) changed |= strip(h);
    for (auto& h : m_forward) changed |= strip(h);
    for (auto& [name, refs] : m_sets) changed |= strip(refs);
    // History states that became identical to their neighbour are noise; drop empty duplicates.
    std::erase_if(m_back, [](const std::vector<ObjRef>& h) { return h.empty(); });
    std::erase_if(m_forward, [](const std::vector<ObjRef>& h) { return h.empty(); });
    if (changed) ++m_version;
}

} // namespace helios::tf
