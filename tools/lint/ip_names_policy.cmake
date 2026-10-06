# Names the IP lint (tools/lint/ip_names.cmake) keeps out of the tree (01 §4.1 rule 2, §5.2;
# CLAUDE.md "Legal / IP hygiene"). Word matches, including words inside identifiers (camelCase,
# snake_case, UPPER_CASE) and paths; reference names of 5+ letters in any case. Keep this list
# distinctive: common English words (Hollow, Quiet, Ember, Mule, Vane, Guardian, Ghost) would drown
# the lint in false positives.

# Reference-content names (*Cinder Reach*, 01 §4.2). Allowed only in the content directories (below);
# engine and Foundation code must stay setting-agnostic. Case-sensitive.
set(HELIOS_IP_REFERENCE_NAMES
  "Cinder Reach" Tallis Harrow "Harrow High" Saltmarch Kestrel Keth "Meridian Directorate" "Free Compact"
  "Osk Yard" "Lattice Heart" Osk)

# In-universe names of other franchises (Star Wars incl. SWG and SWTOR, EVE Online, Destiny, Star
# Citizen): forbidden everywhere the lint looks, content and code alike.
# Case-insensitive (lower case here):
set(HELIOS_IP_FRANCHISE_NAMES_ANYCASE
  jedi sith wookiee wookie tatooine lightsaber corellia corellian naboo coruscant skywalker stormtrooper
  mandalorian "mos eisley" "twi'lek" ithorian trandoshan "mon calamari" "x-wing" "tie fighter" "millennium falcon"
  caldari amarr gallente minmatar "concord" "jita" "capsuleer"
  "cayde" "zavala" "ikora" "savathun" "the traveler"
  vanduul hurston arccorp microtech "crusader industries" "aegis dynamics" "drake interplanetary" "roberts space industries")
# Case-sensitive (proper nouns that are ordinary words or identifiers in lower case):
set(HELIOS_IP_FRANCHISE_NAMES_CASED Vader Yoda Jawa Ewok Hutt Jove Oryx Banu "Old Republic")

# Franchise titles: fine in code comments that credit an architecture ("EVE's time dilation"), but
# never in shipped sample content or concept art. Case-insensitive, checked in the content directories only.
set(HELIOS_IP_FRANCHISE_TITLES "star wars" swtor swg "eve online" "star citizen" "destiny 2" "squadron 42")

# Where the lint looks (relative to the repository root) and what it skips. The content directories
# may use reference-content names and must not use franchise titles. docs/concept/ (concept art: sidecars
# with their AI prompts, and reviews) is one of them: it names the reference setting as docs/plan/ does
# (which the lint does not scan), and its prompts must not reach for other franchises by name or title.
set(HELIOS_IP_SCAN_DIRS engine apps tools schemas services shaders content gems cmake docs/concept)
set(HELIOS_IP_CONTENT_DIRS content docs/concept)
# Top-level files that are the reference game's own (WP-0.20): its project file names the product
# (cinder-reach, 08 §2.10.1) and its zones (tallis). They are content too, so franchise titles stay out of them.
set(HELIOS_IP_CONTENT_FILES helios.project.jsonc)
# tools/lint/ itself is skipped: its policy files, fixtures and tests spell the names out.
set(HELIOS_IP_EXCLUDE_PATTERNS
  "^tools/lint/"
  "^tools/prebuilt/" "^tools/vendor/" "(^|/)\\.git/" "(^|/)build/")
# Content text formats too (WP-0.20): records, containers, entities and their `.meta` provenance sidecars.
set(HELIOS_IP_TEXT_EXTENSIONS
  .h .hpp .hh .inl .c .cc .cpp .cxx .cmake .txt .go .mod .hschema .jsonc .json .lua .luau .slang .hlsl .glsl
  .md .yaml .yml .toml .rml .rcss .py .sh .ps1 .in .manifest .rc .xml .csv .ini .cfg .sql .proto
  .hrec .hcont .hent .meta)

# Waivers: "<path>|<name>|<reason>" (a path ending in '/' covers a directory). No ';' in reasons.
# Every waiver below is engine code that predates the lint (or its identifier-form matching) and
# is listed in the WP-0.2 review report for its owner to rename; remove the waiver with the rename.
set(HELIOS_IP_WAIVERS
  "engine/ecs/tests/test_command_buffer.cpp|Kestrel|ECS test data predating the rule. The ECS owner renames it (WP-0.2 report). Remove this waiver then."
  "engine/reflect/include/helios/reflect/record.h|Kestrel|Doc example record name 'hull/kestrel' in a public header (reflect owner renames it, WP-0.2 review report)."
  "engine/reflect/include/helios/reflect/types.h|Kestrel|Doc example localization key 'ship.kestrel.name' (reflect owner renames it, WP-0.2 review report)."
  "engine/reflect/tests/test_record.cpp|Kestrel|Test record name 'hull/kestrel' (reflect owner renames it, WP-0.2 review report)."
  "engine/reflect/tests/test_types.cpp|Kestrel|Test localization key 'ship.kestrel.name' (reflect owner renames it, WP-0.2 review report)."
  "engine/authority/tests/test_lease_ag.cpp|Tallis|Test zone names 'tallis' (authority owner renames them, WP-0.2 review report)."
  "engine/authority/tests/test_lease_ag.cpp|Harrow|Test zone name 'harrow' (authority owner renames it, WP-0.2 review report)."
  "engine/server/|Tallis|Default and test zone names ('tallis', e.g. GatewayConfig::defaultZone) in the cell/gateway runtime, in development. The server owner replaces them with neutral names or project config (WP-0.2 review report)."
  "engine/server/|Harrow|Test zone names ('harrow') in the cell/gateway runtime, in development. The server owner renames them (WP-0.2 review report)."
  "apps/cellserver/|Tallis|Default zone name 'tallis' in the cell server executable. The server owner moves it to project config (WP-0.2 review report)."
  "apps/gateway/|Tallis|Default zone name 'tallis' in the gateway executable. The server owner moves it to project config (WP-0.2 review report).")
