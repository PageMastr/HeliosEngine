#pragma once
// Part of the ok fixture: GIVEN, WHEN and THEN not followed by '(' are not test macros, even at
// global scope in a header.
enum class Step { GIVEN, WHEN, THEN };
