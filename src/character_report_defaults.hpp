#pragma once
#define AION_REPORT_DEFAULT_URL "https://mmorpgchat.com"
// Private build-time defaults are deliberately kept outside the source package.
#if __has_include("../private/character-report-defaults.hpp")
#include "../private/character-report-defaults.hpp"
#endif
#ifndef AION_REPORT_DEFAULT_TOKEN
#define AION_REPORT_DEFAULT_TOKEN ""
#endif
