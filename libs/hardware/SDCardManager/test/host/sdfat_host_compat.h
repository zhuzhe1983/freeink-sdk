#pragma once

// SdFat's non-Arduino PrintBasic still names this Arduino forward declaration.
class __FlashStringHelper;

// Used only by SdFat's short-name collision handling, not by these read tests.
inline unsigned long millis() { return 0; }
