// Process-lifetime storage for builder-reflected types.

#include "helios/reflect/builder.h"

#include <deque>
#include <memory>
#include <mutex>

namespace helios::refl::detail {

namespace {
/// Everything a built TypeInfo points to. Never freed: TypeInfos live for the whole process.
struct BuiltType {
    std::string name;
    std::string doc;
    std::deque<std::string> strings;
    std::vector<FieldInfo> fields;
    std::vector<std::vector<std::string_view>> was;
    std::vector<EnumValue> enumValues;
    std::vector<VariantAlt> alternatives;
    std::vector<std::vector<AttrArg>> attrArgs;
    std::vector<Attr> attrs;
    std::vector<std::vector<Attr>> fieldAttrs;
    std::deque<attrs::Keyed> keyed;
    TypeInfo info;

    std::string_view intern(std::string_view s) { return strings.emplace_back(s); }
};

struct Arena {
    std::mutex mutex;
    std::deque<BuiltType> types;
};

Arena& arena() {
    static Arena* a = new Arena(); // intentionally leaked: TypeInfos must outlive static destructors
    return *a;
}
} // namespace

TypeBuilderCore::TypeBuilderCore(std::string_view qualifiedName, Kind kind, DeclKind decl)
    : m_name(qualifiedName), m_kind(kind), m_decl(decl) {}

void TypeBuilderCore::addField(std::string_view name, u32 offset, TypeFn type, const FieldOptions& options,
                               const void* defaultValue) {
    FieldOptions o = options;
    if (o.id == 0) o.id = m_nextId;
    m_nextId = o.id + 1;
    m_fields.push_back(PendingField{std::string(name), offset, type, std::move(o), defaultValue});
}

void TypeBuilderCore::addEnumValue(std::string_view name, i64 value) { m_enumValues.emplace_back(std::string(name), value); }

void TypeBuilderCore::addAlternative(std::string_view name, u32 id, TypeFn type) {
    m_alternatives.push_back(PendingAlt{std::string(name), id == 0 ? static_cast<u32>(m_alternatives.size() + 1) : id, type});
}

void TypeBuilderCore::setDoc(std::string_view doc) { m_doc = doc; }

void TypeBuilderCore::addAttr(BuilderAttr attr) { m_attrs.push_back(std::move(attr)); }

const TypeInfo* TypeBuilderCore::build(u32 size, u32 align, const TypeOps& ops, TypeFn underlying) {
    Arena& a = arena();
    std::lock_guard lock(a.mutex);
    BuiltType& b = a.types.emplace_back();
    b.name = m_name;
    b.doc = m_doc;
    // Reserve up front: FieldInfo/Attr spans point into these vectors.
    b.fields.reserve(m_fields.size());
    b.was.reserve(m_fields.size());
    b.fieldAttrs.reserve(m_fields.size());
    usize argLists = m_attrs.size();
    for (const PendingField& pf : m_fields) argLists += pf.options.attrs.size() + (pf.options.keyedBy.empty() ? 0 : 1);
    b.attrArgs.reserve(argLists);
    b.attrs.reserve(m_attrs.size());
    for (const BuilderAttr& attr : m_attrs) {
        std::vector<AttrArg>& args = b.attrArgs.emplace_back();
        for (const auto& [k, v] : attr.args) args.push_back(AttrArg{b.intern(k), b.intern(v)});
        Attr out;
        out.name = b.intern(attr.name);
        out.args = args;
        b.attrs.push_back(out);
    }
    u64 layout = fnv1a64(m_name);
    for (const PendingField& pf : m_fields) {
        std::vector<std::string_view>& was = b.was.emplace_back();
        for (const std::string& w : pf.options.was) was.push_back(b.intern(w));
        FieldInfo f;
        f.name = b.intern(pf.name);
        f.id = pf.options.id;
        f.offset = pf.offset;
        f.typeFn = pf.type;
        f.flags = pf.options.flags;
        std::vector<Attr>& fattrs = b.fieldAttrs.emplace_back();
        for (const BuilderAttr& attr : pf.options.attrs) {
            std::vector<AttrArg>& args = b.attrArgs.emplace_back();
            for (const auto& [k, v] : attr.args) args.push_back(AttrArg{b.intern(k), b.intern(v)});
            Attr out;
            out.name = b.intern(attr.name);
            out.args = args;
            fattrs.push_back(out);
        }
        if (!pf.options.keyedBy.empty()) {
            std::vector<AttrArg>& args = b.attrArgs.emplace_back();
            args.push_back(AttrArg{{}, b.intern(pf.options.keyedBy)});
            const attrs::Keyed& k = b.keyed.emplace_back(attrs::Keyed{args.back().value});
            fattrs.push_back(Attr{"keyed", args, &k, attrs::Keyed::kKind});
        }
        f.attrs = fattrs;
        if (!pf.options.keyedBy.empty()) f.flags |= FieldFlags::Keyed;
        f.was = was;
        f.defaultValue = pf.defaultValue;
        f.doc = b.intern(pf.options.doc);
        b.fields.push_back(f);
        layout = hashCombine(layout, hashCombine(fnv1a64(pf.name), f.id));
    }
    for (const auto& [name, value] : m_enumValues) b.enumValues.push_back(EnumValue{b.intern(name), value, {}});
    for (const PendingAlt& alt : m_alternatives) b.alternatives.push_back(VariantAlt{b.intern(alt.name), alt.id, alt.type});

    TypeInfo& t = b.info;
    t.qualifiedName = b.name;
    const usize dot = b.name.rfind('.');
    t.name = dot == std::string::npos ? std::string_view(b.name) : std::string_view(b.name).substr(dot + 1);
    t.id = typeIdFromName(b.name);
    t.size = size;
    t.align = align;
    t.kind = m_kind;
    t.wire = (m_kind == Kind::Enum || m_kind == Kind::Flags) ? WireType::Varint : WireType::Len;
    t.decl = m_decl;
    t.flags = m_fields.empty() && m_kind == Kind::Struct ? TypeFlags::Unit : TypeFlags::None;
    t.layoutHash = layout;
    t.fields = b.fields;
    t.enumValues = b.enumValues;
    t.alternatives = b.alternatives;
    t.elementFn = underlying;
    t.ops = &ops;
    t.attrs = b.attrs;
    t.doc = b.doc;
    return &t;
}

} // namespace helios::refl::detail
