# Licence policy for tools/lint/licenses.cmake (CLAUDE.md "Legal / IP hygiene", 01 §5.2, ADR-012,
# 09 §5.3).
#
# Allowed in shipped code: MIT, BSD, zlib, Apache-2.0, Boost (BSL-1.0), public domain / CC0, and —
# per 01 §5.2, which lists the scanner's allowlist explicitly — ISC and the PostgreSQL licence. SIL
# OFL-1.1 is allowed for font assets only (ADR-010). Everything else (GPL, LGPL, AGPL, MPL, EPL,
# SSPL, BUSL, Artistic, Commons Clause, CC-BY-SA/NC/ND, the JSON licence, unknown text) fails the
# lint unless waived below with a reason. Changing this file is a legal decision: it needs the
# licence sign-off of 09 §4.3 (H4).

# Families and the phrase that identifies each licence text (matched on normalized text: lower
# case, comment markers removed, whitespace collapsed). Keep the phrases specific: a loose one
# (plain "public domain") let the Artistic License ("place your modifications in the Public
# Domain") pass as public domain.
set(HELIOS_LICENSE_FAMILIES MIT BSD zlib Apache-2.0 BSL-1.0 PublicDomain ISC PostgreSQL)
set(HELIOS_LICENSE_FP_MIT "permission is hereby granted, free of charge, to any person obtaining a copy")
set(HELIOS_LICENSE_FP_BSD "redistribution and use in source and binary forms, with or without modification, are permitted")
set(HELIOS_LICENSE_FP_zlib "provided 'as-is', without any express or implied warranty.*permission is granted to anyone to use this software for any purpose")
set(HELIOS_LICENSE_FP_Apache-2.0 "apache license,? version 2\\.0|apache-2\\.0")
set(HELIOS_LICENSE_FP_BSL-1.0 "boost software license")
set(HELIOS_LICENSE_FP_PublicDomain
  "unlicense\\.org|free and unencumbered software released into the public domain|creative commons (legal code )?cc0|cc0 1\\.0 universal|cc0-1\\.0|dedicated all copyright[a-z ,]* to the public domain|the authors? disclaims? copyright to this source code|(this|the) (software|code|file|library|work|source code) (is|are) (hereby )?(released |placed |dedicated )?(in|into|to) the public domain")
set(HELIOS_LICENSE_FP_ISC "permission to use, copy, modify, and(/or)? distribute this software for any purpose with or without fee is hereby granted")
set(HELIOS_LICENSE_FP_PostgreSQL "permission to use, copy, modify, and distribute this software and its documentation for any purpose, without fee, and without a written agreement is hereby granted")
# Font-only family.
set(HELIOS_LICENSE_FP_OFL "sil open font license")
set(HELIOS_LICENSE_FONT_PATH_PATTERN "(^|/)fonts?/")

# Copyleft or otherwise disallowed licences. Checked even when an allowed family also matched (a
# licence that adds restrictions to MIT-style text is not MIT).
set(HELIOS_LICENSE_FORBIDDEN
  "gnu general public license|gnu lesser general public license|gnu library general public license|gnu affero general public license|gnu free documentation license|mozilla public license|eclipse public license|common development and distribution license|server side public license|business source license|commons clause|artistic license|attribution-sharealike|attribution-noncommercial|attribution-noderivs|attribution-noderivatives|shall be used for good, not evil|elastic license|polyform|source available license|(^|[^a-z])(a|l)?gpl(v?[0-9.]*)?([^a-z]|$)")
# A file that names a forbidden licence passes only as one option of an explicit multi-licence
# choice that also offers an allowed licence by name. No bare "either" or "at your option": the GPL
# itself says "either version 2 ... or (at your option) any later version".
set(HELIOS_LICENSE_CHOICE
  "dual[- ]licen[cs]ed|dual licen[cs]e|tri-licen[cs]ed|multi-licen[cs]ed|(your|at your) choice of|choice of (the following|one of|either)|choose (the license|whichever|one of|any of)|(one|any) of (the following|three|two|these) licen[cs]es|licen[cs]ed under (either|one of|any of)")
set(HELIOS_LICENSE_ALLOWED_NAMES
  "mit licen[cs]e|bsd([- ]style|[- ][0-9][- ]clause)?[ -]licen[cs]e|bsd-[0-9]|zlib licen[cs]e|apache licen[cs]e|apache-2|boost software licen[cs]e|unlicense|cc0|isc licen[cs]e")

# Identifiers accepted in the licence column of third_party/MANIFEST.md.
set(HELIOS_LICENSE_MANIFEST_IDS MIT MIT-0 BSD BSD-2 BSD-3 zlib Apache-2.0 Boost BSL-1.0 CC0 "public domain" Unlicense
                                ISC PostgreSQL)

# Third-party directories whose MANIFEST.md row uses a different name.
set(HELIOS_LICENSE_MANIFEST_ALIASES "vma=VulkanMemoryAllocator")

# Waivers: "<path relative to third_party>|<reason>" for licence files, and
# "MANIFEST:<dir>:<identifier>|<reason>" for MANIFEST.md licence identifiers.
set(HELIOS_LICENSE_WAIVERS
  "sdl3/src/hidapi/LICENSE-gpl3.txt|HIDAPI inside SDL3 is tri-licensed (GPL-3.0, BSD, original HIDAPI licence, see LICENSE.txt next to it). Helios uses it under the BSD option."
  "sdl3/src/hidapi/LICENSE-orig.txt|The original HIDAPI licence (use for any purpose, keep the copyright notice) is one of HIDAPI's three options. Helios uses the BSD option.")
