#pragma once
#include "GalacticRacerLetterboxingPolicy.hpp"

namespace uevr::swgr_letterbox {
void update(bool requested, bool supported);
void shutdown();
Status status();
const char* status_text(Status value);
}
